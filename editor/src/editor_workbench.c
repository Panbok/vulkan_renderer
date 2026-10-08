#include "editor_workbench.h"

#include "editor_internal.h"
#include "editor_level.h"
#include "editor_projects.h"

#include "core/vkr_json.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"
#include "renderer/systems/vkr_gizmo_system.h"

#include <stdio.h>

_Static_assert(VKR_EDITOR_WINDOW_COUNT <= 32u,
               "A workbench keeps its open windows in one 32-bit mask");

typedef struct WorkbenchInfo {
  const char *id;
  const char *name;
  VkrUiIcon icon;
} WorkbenchInfo;

static const WorkbenchInfo s_workbenches[VKR_EDITOR_WORKBENCH_COUNT] = {
    [VKR_EDITOR_WORKBENCH_GENERAL] = {"general", "General", VKR_UI_ICON_LAYOUT},
    [VKR_EDITOR_WORKBENCH_LEVEL_DESIGN] = {"level_design", "Level Design",
                                           VKR_UI_ICON_SHAPES},
    [VKR_EDITOR_WORKBENCH_TERRAIN] = {"terrain", "Terrain", VKR_UI_ICON_WAVES},
    [VKR_EDITOR_WORKBENCH_LIGHTING] = {"lighting", "Lighting",
                                       VKR_UI_ICON_LIGHT},
    [VKR_EDITOR_WORKBENCH_SCRIPTING] = {"scripting", "Scripting",
                                        VKR_UI_ICON_CODE},
};

/* Floating windows whose body a dock panel can host instead. */
static const struct {
  VkrEditorWindowKind window;
  VkrUiDockPanelKind panel;
} s_docked_windows[] = {
    {VKR_EDITOR_WINDOW_LEVEL, VKR_UI_DOCK_PANEL_LEVEL_CHECKS},
    {VKR_EDITOR_WINDOW_SCRIPT, VKR_UI_DOCK_PANEL_SCRIPT},
    {VKR_EDITOR_WINDOW_TERRAIN, VKR_UI_DOCK_PANEL_TERRAIN},
};

static const char *const s_scene_tools[VKR_EDITOR_SCENE_TOOL_COUNT] = {
    "none", "brush_draw", "clip", "terrain", "stairs", "corridor", "measure"};
static const char *const s_snap_targets[VKR_EDITOR_SNAP_COUNT] = {
    "free", "surface", "grid"};

/* The top bar and the workbench row. */
static float32_t workbench_chrome_pt(void) {
  return VKR_UI_DOCK_TOOLBAR_PT + VKR_EDITOR_WORKBENCH_ROW_PT;
}

static VkrUiDockTab workbench_tab(VkrUiDockPanelKind kind) {
  return (VkrUiDockTab){.id = (uint64_t)kind + 1u, .panel_kind = kind};
}

static void workbench_leaf(VkrUiDockTree *tree, uint32_t index, uint32_t parent,
                           VkrUiDockPanelKind kind) {
  tree->nodes[index] = (VkrUiDockNode){
      .kind = VKR_UI_DOCK_NODE_TABS,
      .parent = parent,
      .used = true_v,
      .as.leaf = {.tabs = {workbench_tab(kind)},
                  .tab_count = 1u,
                  .active_tab = 0u},
  };
}

static void workbench_split(VkrUiDockTree *tree, uint32_t index,
                            uint32_t parent, VkrUiDockSplitAxis axis,
                            float32_t ratio, uint32_t first, uint32_t second) {
  tree->nodes[index] = (VkrUiDockNode){
      .kind = VKR_UI_DOCK_NODE_SPLIT,
      .parent = parent,
      .used = true_v,
      .as.split = {.axis = axis,
                   .ratio = ratio,
                   .first = first,
                   .second = second},
  };
}

/* An empty tree of `nodes` nodes with the top bar as node 1 under split
   node 0, whose second child is node 2. */
static void workbench_tree_begin(VkrUiDockTree *tree, uint32_t nodes) {
  MemZero(tree, sizeof(*tree));
  tree->root = 0u;
  tree->node_high_water = nodes;
  tree->revision = 1u;
  tree->focused_tab_id = workbench_tab(VKR_UI_DOCK_PANEL_SCENE_VIEWPORT).id;
  tree->splitter_px = VKR_UI_DOCK_SPLITTER_PT;
  tree->tab_bar_px = VKR_UI_DOCK_TAB_BAR_PT;
  tree->interaction = (VkrUiDockInteraction){
      .tab_leaf = VKR_UI_DOCK_NODE_NONE,
      .resize_split = VKR_UI_DOCK_NODE_NONE,
      .drop_leaf = VKR_UI_DOCK_NODE_NONE,
  };
  workbench_split(tree, 0u, VKR_UI_DOCK_NODE_NONE, VKR_UI_DOCK_SPLIT_Y, 0.06f,
                  1u, 2u);
  workbench_leaf(tree, 1u, 0u, VKR_UI_DOCK_PANEL_TOOLBAR);
}

/* `count` tabs in leaf `leaf`, the first one shown. */
static void workbench_tabs(VkrUiDockTree *tree, uint32_t leaf, uint32_t parent,
                           const VkrUiDockPanelKind *kinds, uint32_t count) {
  workbench_leaf(tree, leaf, parent, kinds[0]);
  for (uint32_t i = 1u; i < count; ++i) {
    tree->nodes[leaf].as.leaf.tabs[i] = workbench_tab(kinds[i]);
  }
  tree->nodes[leaf].as.leaf.tab_count = count;
}

/* `side` left of the Scene with the window share `side_share`, the `bottom`
   tabs under the Scene, and the Outliner over Details on the right with the
   Outliner's share `outliner`. */
static void workbench_side_layout(VkrUiDockTree *tree, VkrUiDockPanelKind side,
                                  float32_t side_share,
                                  const VkrUiDockPanelKind *bottom,
                                  uint32_t bottom_count, float32_t outliner) {
  workbench_tree_begin(tree, 11u);
  workbench_split(tree, 2u, 0u, VKR_UI_DOCK_SPLIT_X, side_share, 3u, 4u);
  workbench_leaf(tree, 3u, 2u, side);
  workbench_split(tree, 4u, 2u, VKR_UI_DOCK_SPLIT_X, 0.79f, 5u, 6u);
  workbench_split(tree, 5u, 4u, VKR_UI_DOCK_SPLIT_Y, 0.74f, 7u, 8u);
  workbench_leaf(tree, 7u, 5u, VKR_UI_DOCK_PANEL_SCENE_VIEWPORT);
  workbench_tabs(tree, 8u, 5u, bottom, bottom_count);
  workbench_split(tree, 6u, 4u, VKR_UI_DOCK_SPLIT_Y, outliner, 9u, 10u);
  workbench_leaf(tree, 9u, 6u, VKR_UI_DOCK_PANEL_HIERARCHY);
  workbench_leaf(tree, 10u, 6u, VKR_UI_DOCK_PANEL_INSPECTOR);
}

/* The Script editor over the Console on the left, the Scene over the
   Outliner and Details on the right. */
static void workbench_scripting_layout(VkrUiDockTree *tree) {
  workbench_tree_begin(tree, 11u);
  workbench_split(tree, 2u, 0u, VKR_UI_DOCK_SPLIT_X, 0.6f, 3u, 4u);
  workbench_split(tree, 3u, 2u, VKR_UI_DOCK_SPLIT_Y, 0.72f, 5u, 6u);
  workbench_leaf(tree, 5u, 3u, VKR_UI_DOCK_PANEL_SCRIPT);
  workbench_leaf(tree, 6u, 3u, VKR_UI_DOCK_PANEL_CONSOLE);
  workbench_split(tree, 4u, 2u, VKR_UI_DOCK_SPLIT_Y, 0.45f, 7u, 8u);
  workbench_leaf(tree, 7u, 4u, VKR_UI_DOCK_PANEL_SCENE_VIEWPORT);
  workbench_split(tree, 8u, 4u, VKR_UI_DOCK_SPLIT_Y, 0.45f, 9u, 10u);
  workbench_leaf(tree, 9u, 8u, VKR_UI_DOCK_PANEL_HIERARCHY);
  workbench_leaf(tree, 10u, 8u, VKR_UI_DOCK_PANEL_INSPECTOR);
}

static void workbench_builtin_layout(uint32_t index, VkrUiDockTree *tree) {
  static const VkrUiDockPanelKind level_design[] = {
      VKR_UI_DOCK_PANEL_LEVEL_CHECKS, VKR_UI_DOCK_PANEL_CONTENT,
      VKR_UI_DOCK_PANEL_CONSOLE};
  static const VkrUiDockPanelKind terrain[] = {VKR_UI_DOCK_PANEL_CONTENT,
                                               VKR_UI_DOCK_PANEL_CONSOLE};
  static const VkrUiDockPanelKind lighting[] = {VKR_UI_DOCK_PANEL_CONSOLE};
  switch (index) {
  case VKR_EDITOR_WORKBENCH_LEVEL_DESIGN:
    workbench_side_layout(tree, VKR_UI_DOCK_PANEL_TOOLS, 0.14f, level_design,
                          ArrayCount(level_design), 0.42f);
    break;
  case VKR_EDITOR_WORKBENCH_TERRAIN:
    /* Wide enough for the five sculpt modes' labels. */
    workbench_side_layout(tree, VKR_UI_DOCK_PANEL_TERRAIN, 0.2f, terrain,
                          ArrayCount(terrain), 0.42f);
    break;
  case VKR_EDITOR_WORKBENCH_LIGHTING:
    /* Lights are tuned in Details, so it takes most of the column. */
    workbench_side_layout(tree, VKR_UI_DOCK_PANEL_TOOLS, 0.14f, lighting,
                          ArrayCount(lighting), 0.32f);
    break;
  case VKR_EDITOR_WORKBENCH_SCRIPTING:
    workbench_scripting_layout(tree);
    break;
  default:
    vkr_ui_dock_default_editor_layout(tree);
    break;
  }
  tree->toolbar_pt = workbench_chrome_pt();
}

_Static_assert(VKR_EDITOR_WORKBENCH_COUNT <= VKR_EDITOR_WORKBENCH_MAX,
               "Every built-in workbench has a tab");
_Static_assert(CMD_WORKBENCH_9 - CMD_WORKBENCH_1 + 1 ==
                   VKR_EDITOR_WORKBENCH_MAX,
               "Each tab position has a command");

void vkr_editor_workbench_init(VkrEditorWorkbenches *workbenches) {
  MemZero(workbenches, sizeof(*workbenches));
  workbenches->count = VKR_EDITOR_WORKBENCH_COUNT;
  for (uint32_t i = 0u; i < VKR_EDITOR_WORKBENCH_COUNT; ++i) {
    VkrEditorWorkbench *item = &workbenches->items[i];
    item->kind = (VkrEditorWorkbenchKind)i;
    snprintf(item->id, sizeof(item->id), "%s", s_workbenches[i].id);
    snprintf(item->name, sizeof(item->name), "%s", s_workbenches[i].name);
    workbench_builtin_layout(i, &item->layout);
  }
  workbenches->items[VKR_EDITOR_WORKBENCH_GENERAL].mode =
      (VkrEditorWorkbenchMode){.gizmo_tool = VKR_GIZMO_MODE_TRANSLATE,
                               .snap = VKR_EDITOR_SNAP_SURFACE,
                               .grid = true_v};
  /* Blockout starts from the Select tool on the grid; drawing waits for B
     or the palette, so entering never turns a click into a brush. */
  workbenches->items[VKR_EDITOR_WORKBENCH_LEVEL_DESIGN].mode =
      (VkrEditorWorkbenchMode){.gizmo_tool = VKR_GIZMO_MODE_NONE,
                               .snap = VKR_EDITOR_SNAP_GRID,
                               .grid = true_v};
  /* Sculpting is the Terrain workbench's task, so its tool is on. */
  workbenches->items[VKR_EDITOR_WORKBENCH_TERRAIN].mode =
      (VkrEditorWorkbenchMode){.gizmo_tool = VKR_GIZMO_MODE_NONE,
                               .scene_tool = VKR_EDITOR_SCENE_TOOL_TERRAIN,
                               .snap = VKR_EDITOR_SNAP_SURFACE};
  workbenches->items[VKR_EDITOR_WORKBENCH_LIGHTING].mode =
      (VkrEditorWorkbenchMode){.gizmo_tool = VKR_GIZMO_MODE_TRANSLATE,
                               .snap = VKR_EDITOR_SNAP_SURFACE};
  workbenches->items[VKR_EDITOR_WORKBENCH_SCRIPTING].mode =
      (VkrEditorWorkbenchMode){.gizmo_tool = VKR_GIZMO_MODE_TRANSLATE,
                               .snap = VKR_EDITOR_SNAP_SURFACE,
                               .grid = true_v};
  workbenches->active = VKR_EDITOR_WORKBENCH_GENERAL;
  workbenches->requested = UINT32_MAX;
  workbenches->renaming = UINT32_MAX;
  workbenches->next_custom = 1u;
  workbenches->click_tab = UINT32_MAX;
}

const char *vkr_editor_workbench_id(const VkrEditorWorkbenches *workbenches,
                                    uint32_t index) {
  return index < workbenches->count ? workbenches->items[index].id : "";
}

const char *vkr_editor_workbench_name(const VkrEditorWorkbenches *workbenches,
                                      uint32_t index) {
  return index < workbenches->count ? workbenches->items[index].name : "";
}

static bool8_t workbench_word_is(String8 word, const char *text) {
  const uint64_t length = strlen(text);
  if (word.length != length) {
    return false_v;
  }
  for (uint64_t i = 0u; i < length; ++i) {
    char c = (char)word.str[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c - 'A' + 'a');
    }
    char t = text[i];
    if (t >= 'A' && t <= 'Z') {
      t = (char)(t - 'A' + 'a');
    }
    /* "level design" and "level_design" name the same workbench. */
    if (c == ' ') {
      c = '_';
    }
    if (t == ' ') {
      t = '_';
    }
    if (c != t) {
      return false_v;
    }
  }
  return true_v;
}

uint32_t vkr_editor_workbench_find(const VkrEditorWorkbenches *workbenches,
                                   String8 word) {
  const uint32_t count = workbenches->count;
  if (workbench_word_is(word, "next")) {
    return (workbenches->active + 1u) % count;
  }
  if (workbench_word_is(word, "prev") || workbench_word_is(word, "previous")) {
    return (workbenches->active + count - 1u) % count;
  }
  if (word.length == 1u && word.str[0] >= '1' &&
      (uint32_t)(word.str[0] - '1') < count) {
    return (uint32_t)(word.str[0] - '1');
  }
  for (uint32_t i = 0u; i < count; ++i) {
    if (workbench_word_is(word, workbenches->items[i].id) ||
        workbench_word_is(word, workbenches->items[i].name)) {
      return i;
    }
  }
  return UINT32_MAX;
}

/* A pointer gesture that a switch would cut off. */
static bool8_t workbench_gesture(const VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame) {
  const VkrUiDockInteraction *dock = &frame->dock->interaction;
  return dock->dragging_tab || dock->resize_split != VKR_UI_DOCK_NODE_NONE ||
         input_is_button_down(frame->input, BUTTON_LEFT) ||
         editor->face_dragging || editor->brush_dragging ||
         editor->brush_raising;
}

bool8_t vkr_editor_workbench_request(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     uint32_t index, char *message,
                                     uint64_t capacity) {
  char unused[8];
  if (!message) {
    message = unused;
    capacity = sizeof(unused);
  }
  if (index >= editor->workbenches.count) {
    snprintf(message, capacity, "No such workbench");
    return false_v;
  }
  if (frame->scene_maximized) {
    snprintf(message, capacity, "Restore the maximized Scene first");
    return false_v;
  }
  if (frame->scene_only) {
    snprintf(message, capacity, "The Scene-only view has no workbenches");
    return false_v;
  }
  if (workbench_gesture(editor, frame)) {
    snprintf(message, capacity, "Finish the drag before switching workbenches");
    return false_v;
  }
  editor->workbenches.requested = index;
  snprintf(message, capacity, "Switching to %s",
           editor->workbenches.items[index].name);
  return true_v;
}

static uint32_t workbench_open_windows(const VkrEditorUi *editor) {
  uint32_t windows = 0u;
  for (uint32_t i = 0u; i < VKR_EDITOR_WINDOW_COUNT; ++i) {
    if (editor->windows[i].visible) {
      windows |= 1u << i;
    }
  }
  return windows;
}

static VkrEditorWorkbenchMode
workbench_current_mode(const VkrEditorUi *editor,
                       const VkrSampleUiFrame *frame) {
  const VkrEditorSceneTool tool = vkr_editor_scene_tool(editor);
  return (VkrEditorWorkbenchMode){
      .gizmo_tool = frame->view_state.gizmo_tool,
      .scene_tool = tool,
      .snap = editor->placement.target,
      .grid = frame->view_state.grid_enabled,
  };
}

const VkrUiDockTree *vkr_editor_workbench_layout(const VkrEditorUi *editor,
                                                 const VkrSampleUiFrame *frame,
                                                 uint32_t index) {
  return index == editor->workbenches.active
             ? frame->dock
             : &editor->workbenches.items[index].layout;
}

uint32_t vkr_editor_workbench_windows(const VkrEditorUi *editor,
                                      uint32_t index) {
  return index == editor->workbenches.active
             ? workbench_open_windows(editor)
             : editor->workbenches.items[index].windows;
}

VkrEditorWorkbenchMode vkr_editor_workbench_mode(const VkrEditorUi *editor,
                                                 const VkrSampleUiFrame *frame,
                                                 uint32_t index) {
  return index == editor->workbenches.active
             ? workbench_current_mode(editor, frame)
             : editor->workbenches.items[index].mode;
}

const char *vkr_editor_scene_tool_name(VkrEditorSceneTool tool) {
  return tool < VKR_EDITOR_SCENE_TOOL_COUNT ? s_scene_tools[tool] : "";
}

const char *vkr_editor_snap_name(VkrEditorSnapTarget snap) {
  return snap < VKR_EDITOR_SNAP_COUNT ? s_snap_targets[snap] : "";
}

static void workbench_apply_mode(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 const VkrEditorWorkbenchMode *mode) {
  /* Pending clicks of the leaving tool end with it. */
  vkr_editor_scene_tool_set(editor, mode->scene_tool);
  editor->placement.target = mode->snap;
  if (frame->view_request) {
    VkrSampleViewState next = frame->view_state;
    next.gizmo_tool = mode->gizmo_tool;
    next.grid_enabled = mode->grid;
    *frame->view_request =
        (VkrSampleViewRequest){.value = next, .apply = true_v};
  }
}

static void workbench_switch(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                             uint32_t target) {
  VkrEditorWorkbenches *workbenches = &editor->workbenches;
  VkrEditorWorkbench *current = &workbenches->items[workbenches->active];
  current->layout = *frame->dock;
  current->windows = workbench_open_windows(editor);
  current->mode = workbench_current_mode(editor, frame);

  const VkrEditorWorkbench *next = &workbenches->items[target];
  const uint64_t revision = frame->dock->revision;
  *frame->dock = next->layout;
  frame->dock->interaction = (VkrUiDockInteraction){
      .tab_leaf = VKR_UI_DOCK_NODE_NONE,
      .resize_split = VKR_UI_DOCK_NODE_NONE,
      .drop_leaf = VKR_UI_DOCK_NODE_NONE,
  };
  frame->dock->revision = revision + 1u;
  for (uint32_t i = 0u; i < VKR_EDITOR_WINDOW_COUNT; ++i) {
    editor->windows[i].visible = (next->windows >> i) & 1u;
  }
  workbench_apply_mode(editor, frame, &next->mode);
  workbenches->active = target;
}

/* Whether the project shows a scene settled enough to follow: no job, load
   or prompt is between scenes. */
static bool8_t workbench_scene_settled(const VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame) {
  return editor->projects && frame->world && !frame->scene_loading &&
         vkr_editor_projects_switch_ready(editor->projects);
}

void vkr_editor_workbench_update(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame) {
  if (frame->scene_only || !frame->dock) {
    return;
  }
  VkrEditorWorkbenches *workbenches = &editor->workbenches;
  frame->dock->toolbar_pt = workbench_chrome_pt();
  /* The active tab keeps the scene the project shows, however it opened:
     the Scenes list, Content, or a switch a prompt cancelled. */
  if (workbench_scene_settled(editor, frame)) {
    VkrEditorWorkbench *active = &workbenches->items[workbenches->active];
    const String8 id = vkr_editor_projects_scene_id(editor->projects);
    snprintf(active->scene_id, sizeof(active->scene_id), "%.*s", (int)id.length,
             (const char *)id.str);
    active->scene_known = true_v;
  }
  const uint32_t target = workbenches->requested;
  workbenches->requested = UINT32_MAX;
  if (target < workbenches->count && target != workbenches->active &&
      !workbench_gesture(editor, frame)) {
    workbench_switch(editor, frame, target);
    /* A tab that shows another scene opens it; unsaved edits ask first. */
    const VkrEditorWorkbench *next = &workbenches->items[target];
    if (next->scene_known && workbench_scene_settled(editor, frame)) {
      const String8 shown = vkr_editor_projects_scene_id(editor->projects);
      if (shown.length != strlen(next->scene_id) ||
          MemCompare(shown.str, next->scene_id, shown.length) != 0) {
        (void)vkr_editor_projects_show_scene(editor->projects, editor, frame,
                                             next->scene_id);
      }
    }
  }
  /* A docked panel is the only host of its window's body: whatever opened
     the window, such as a script double-click, shows the tab instead. */
  for (uint32_t i = 0u; i < ArrayCount(s_docked_windows); ++i) {
    VkrEditorWindowState *window = &editor->windows[s_docked_windows[i].window];
    if (window->visible &&
        vkr_editor_dock_has(frame->dock, s_docked_windows[i].panel)) {
      window->visible = false_v;
      vkr_editor_dock_show(frame->dock, s_docked_windows[i].panel);
    }
  }
}

VkrUiDockPanelKind vkr_editor_window_dock_panel(VkrEditorWindowKind kind) {
  for (uint32_t i = 0u; i < ArrayCount(s_docked_windows); ++i) {
    if (s_docked_windows[i].window == kind) {
      return s_docked_windows[i].panel;
    }
  }
  return VKR_UI_DOCK_PANEL_COUNT;
}

bool8_t vkr_editor_window_shown(const VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame,
                                VkrEditorWindowKind kind) {
  const VkrUiDockPanelKind panel = vkr_editor_window_dock_panel(kind);
  return editor->windows[kind].visible ||
         (panel != VKR_UI_DOCK_PANEL_COUNT && frame->dock &&
          !frame->scene_only &&
          vkr_ui_dock_find_panel(frame->dock, panel, NULL, NULL));
}

void vkr_editor_workbench_reset_index(VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      uint32_t index) {
  VkrEditorWorkbenches *workbenches = &editor->workbenches;
  if (index >= workbenches->count) {
    return;
  }
  const VkrEditorWorkbenchKind kind = workbenches->items[index].kind;
  if (index != workbenches->active) {
    workbench_builtin_layout(kind, &workbenches->items[index].layout);
    return;
  }
  const uint64_t revision = frame->dock->revision;
  workbench_builtin_layout(kind, frame->dock);
  frame->dock->revision = revision + 1u;
}

/* Whether a workbench other than `except` has the name `name`. */
static bool8_t workbench_name_taken(const VkrEditorWorkbenches *workbenches,
                                    const char *name, uint32_t except) {
  for (uint32_t i = 0u; i < workbenches->count; ++i) {
    if (i != except && strcmp(workbenches->items[i].name, name) == 0) {
      return true_v;
    }
  }
  return false_v;
}

bool8_t vkr_editor_workbench_duplicate(VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       uint32_t index, char *message,
                                       uint64_t capacity) {
  VkrEditorWorkbenches *workbenches = &editor->workbenches;
  if (index >= workbenches->count) {
    snprintf(message, capacity, "No such workbench");
    return false_v;
  }
  if (workbenches->count == VKR_EDITOR_WORKBENCH_MAX) {
    snprintf(message, capacity,
             "The row holds %u workbenches; delete one first",
             VKR_EDITOR_WORKBENCH_MAX);
    return false_v;
  }
  VkrEditorWorkbench copy = workbenches->items[index];
  if (index == workbenches->active) {
    copy.layout = *frame->dock;
    copy.windows = workbench_open_windows(editor);
    copy.mode = workbench_current_mode(editor, frame);
  }
  copy.custom = true_v;
  snprintf(copy.id, sizeof(copy.id), "custom_%u", workbenches->next_custom++);
  /* "Level Design (1)", as duplicated objects are numbered. */
  for (uint32_t n = 1u; n < 100u; ++n) {
    snprintf(copy.name, sizeof(copy.name), "%.24s (%u)",
             workbenches->items[index].name, n);
    if (!workbench_name_taken(workbenches, copy.name, UINT32_MAX)) {
      break;
    }
  }
  const uint32_t at = index + 1u;
  for (uint32_t i = workbenches->count; i > at; --i) {
    workbenches->items[i] = workbenches->items[i - 1u];
  }
  workbenches->items[at] = copy;
  workbenches->count++;
  if (workbenches->active >= at) {
    workbenches->active++;
  }
  workbenches->renaming = UINT32_MAX;
  snprintf(message, capacity, "Created %s", copy.name);
  (void)vkr_editor_workbench_request(editor, frame, at, NULL, 0u);
  return true_v;
}

bool8_t vkr_editor_workbench_delete(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame,
                                    uint32_t index, char *message,
                                    uint64_t capacity) {
  VkrEditorWorkbenches *workbenches = &editor->workbenches;
  if (index >= workbenches->count) {
    snprintf(message, capacity, "No such workbench");
    return false_v;
  }
  if (!workbenches->items[index].custom) {
    snprintf(message, capacity,
             "Built-in workbenches stay; Reset panel layout restores one");
    return false_v;
  }
  /* The tab before takes over, else the one after. */
  if (index == workbenches->active) {
    workbench_switch(editor, frame, index > 0u ? index - 1u : index + 1u);
  }
  char name[VKR_EDITOR_WORKBENCH_NAME_CAPACITY];
  snprintf(name, sizeof(name), "%s", workbenches->items[index].name);
  for (uint32_t i = index; i + 1u < workbenches->count; ++i) {
    workbenches->items[i] = workbenches->items[i + 1u];
  }
  workbenches->count--;
  if (workbenches->active > index) {
    workbenches->active--;
  }
  workbenches->requested = UINT32_MAX;
  workbenches->renaming = UINT32_MAX;
  snprintf(message, capacity, "Deleted %s", name);
  return true_v;
}

bool8_t vkr_editor_workbench_move(VkrEditorUi *editor, uint32_t index,
                                  int32_t step, char *message,
                                  uint64_t capacity) {
  VkrEditorWorkbenches *workbenches = &editor->workbenches;
  const int64_t target = (int64_t)index + step;
  if (index >= workbenches->count || target < 0 ||
      target >= (int64_t)workbenches->count) {
    snprintf(message, capacity, "That tab cannot move further");
    return false_v;
  }
  const uint32_t other = (uint32_t)target;
  const VkrEditorWorkbench swap = workbenches->items[index];
  workbenches->items[index] = workbenches->items[other];
  workbenches->items[other] = swap;
  if (workbenches->active == index) {
    workbenches->active = other;
  } else if (workbenches->active == other) {
    workbenches->active = index;
  }
  workbenches->requested = UINT32_MAX;
  workbenches->renaming = UINT32_MAX;
  snprintf(message, capacity, "Moved %s to position %u",
           workbenches->items[other].name, other + 1u);
  return true_v;
}

bool8_t vkr_editor_workbench_rename(VkrEditorUi *editor, uint32_t index,
                                    String8 name, char *message,
                                    uint64_t capacity) {
  VkrEditorWorkbenches *workbenches = &editor->workbenches;
  while (name.length && name.str[0] == ' ') {
    name.str++;
    name.length--;
  }
  while (name.length && name.str[name.length - 1u] == ' ') {
    name.length--;
  }
  if (index >= workbenches->count) {
    snprintf(message, capacity, "No such workbench");
    return false_v;
  }
  if (!name.length || name.length >= VKR_EDITOR_WORKBENCH_NAME_CAPACITY ||
      memchr(name.str, '"', name.length) ||
      memchr(name.str, '\\', name.length)) {
    snprintf(message, capacity,
             "A workbench name has 1 to %u bytes and no quotes or backslashes",
             VKR_EDITOR_WORKBENCH_NAME_CAPACITY - 1u);
    return false_v;
  }
  char text[VKR_EDITOR_WORKBENCH_NAME_CAPACITY];
  snprintf(text, sizeof(text), "%.*s", (int)name.length, name.str);
  if (workbench_name_taken(workbenches, text, index)) {
    snprintf(message, capacity, "Another workbench is named %s", text);
    return false_v;
  }
  MemCopy(workbenches->items[index].name, text, sizeof(text));
  snprintf(message, capacity, "Renamed to %s", text);
  return true_v;
}

void vkr_editor_workbench_rename_begin(VkrEditorUi *editor, uint32_t index) {
  VkrEditorWorkbenches *workbenches = &editor->workbenches;
  if (index >= workbenches->count) {
    return;
  }
  workbenches->renaming = index;
  workbenches->rename_focus = true_v;
  snprintf(workbenches->rename_text, sizeof(workbenches->rename_text), "%s",
           workbenches->items[index].name);
}

void vkr_editor_workbench_reset(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame) {
  vkr_editor_workbench_reset_index(editor, frame, editor->workbenches.active);
  /* The reset layout shows its panels, so a maximized Scene returns. */
  if (frame->scene_maximized_next) {
    *frame->scene_maximized_next = false_v;
  }
}

// =============================================================================
// Row
// =============================================================================

/* The inline name field of the tab being renamed, in column `index`: Enter
   or a click elsewhere keeps the name, Escape drops it. */
static void workbench_rename_field(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   uint32_t index) {
  VkrUiSystem *ui = frame->ui;
  VkrEditorWorkbenches *workbenches = &editor->workbenches;
  VkrUiWidgetConfig field = vkr_ui_widget_config_default();
  field.placement = (VkrUiPlacement){
      .column = index,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_CENTER,
  };
  const VkrUiTheme *theme = vkr_ui_theme();
  field.style.font_size_pt = theme->font_body;
  field.style.padding_pt = (VkrUiEdges){2.0f, 6.0f, 2.0f, 6.0f};
  field.style.background_color = theme->field;
  field.style.border_pt = (VkrUiEdges){1.0f, 1.0f, 1.0f, 1.0f};
  field.style.border_color = theme->accent;
  field.style.corner_radius_pt = (Vec4){4.0f, 4.0f, 4.0f, 4.0f};
  VkrUiTextEditBuffer buffer = {(uint8_t *)workbenches->rename_text,
                                (uint32_t)strlen(workbenches->rename_text),
                                sizeof(workbenches->rename_text)};
  (void)vkr_ui_push_id_u64(ui, index);
  const VkrUiId id =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("rename"));
  /* Focus holds only on a field that has been laid out, so the new field
     takes it on its second build. */
  VkrUiRect laid_out = {0};
  const bool8_t placed = vkr_ui_widget_rect(ui, id, &laid_out);
  if (workbenches->rename_focus && placed) {
    ui->focused_id = id;
    ui->focused_is_text = true_v;
    (void)vkr_ui_keyboard_layer_set(ui, 0u);
  }
  (void)vkr_ui_text_field(ui, string8_lit("rename"), &buffer, &field);
  (void)vkr_ui_pop_id(ui);
  const bool8_t focused = ui->focused_id == id;
  const bool8_t was_focusing = workbenches->rename_focus;
  if (placed) {
    workbenches->rename_focus = false_v;
  }
  if (focused && input_key_just_pressed(frame->input, KEY_ESCAPE)) {
    workbenches->renaming = UINT32_MAX;
    return;
  }
  if ((focused && input_key_just_pressed(frame->input, KEY_ENTER)) ||
      (!focused && !was_focusing)) {
    char message[96];
    if (!vkr_editor_workbench_rename(
            editor, index,
            string8_create_from_cstr((const uint8_t *)workbenches->rename_text,
                                     strlen(workbenches->rename_text)),
            message, sizeof(message))) {
      vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL,
                       vkr_ui_theme()->warning, message);
    }
    workbenches->renaming = UINT32_MAX;
  }
}

void vkr_editor_workbench_build_row(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame) {
  if (frame->scene_only) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  /* The active tab names the open scene; the others show their icons, and
     every tooltip names its workbench. Below the width a label needs, the
     active tab shows its icon too. */
  const bool8_t labels =
      (float32_t)ui->target_width / ui->content_scale >= 520.0f;
  char scene[96];
  vkr_editor_scene_label(editor, frame, scene, sizeof(scene));
  VkrEditorWorkbenches *workbenches = &editor->workbenches;
  const uint32_t count = workbenches->count;
  /* The tabs, the + button and the free space. */
  VkrUiTrack columns[VKR_EDITOR_WORKBENCH_MAX + 2u];
  for (uint32_t i = 0u; i <= count; ++i) {
    columns[i] = (VkrUiTrack){.unit = VKR_UI_TRACK_AUTO};
  }
  if (workbenches->renaming < count) {
    columns[workbenches->renaming] =
        (VkrUiTrack){.value = 150.0f, .unit = VKR_UI_TRACK_PX};
  }
  columns[count + 1u] = (VkrUiTrack){.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack row = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig bar = vkr_ui_panel_config_default();
  bar.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_START,
      .margin_pt = {VKR_EDITOR_NAVIGATION_HEIGHT_PT, 0.0f, 0.0f, 0.0f},
  };
  bar.columns = columns;
  bar.column_count = count + 2u;
  bar.rows = &row;
  bar.row_count = 1u;
  bar.style.padding_pt = (VkrUiEdges){2.0f, 8.0f, 0.0f, 10.0f};
  bar.style.gap_pt = 2.0f;
  bar.style.min_size_pt.y = VKR_EDITOR_WORKBENCH_ROW_PT;
  bar.style.max_size_pt.y = VKR_EDITOR_WORKBENCH_ROW_PT;
  bar.style.border_pt = (VkrUiEdges){0.0f, 0.0f, 1.0f, 0.0f};
  bar.style.background_color = theme->header;
  bar.style.border_color = theme->separator;
  (void)vkr_ui_input_layer_set(ui, 0u);
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.workbenches"), &bar)) {
    return;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrEditorWorkbench *item = &workbenches->items[i];
    const bool8_t active = workbenches->active == i;
    if (workbenches->renaming == i) {
      workbench_rename_field(editor, frame, i);
      continue;
    }
    VkrUiWidgetConfig tab = vkr_ui_widget_config_default();
    tab.placement = (VkrUiPlacement){
        .column = i,
        .row = 0u,
        .column_span = 1u,
        .row_span = 1u,
        .justify = VKR_UI_ALIGN_START,
        .align = VKR_UI_ALIGN_STRETCH,
    };
    tab.style.font_size_pt = theme->font_body;
    tab.text.font = active ? editor->heading_font : VKR_FONT_HANDLE_INVALID;
    tab.style.padding_pt = active && labels
                               ? (VkrUiEdges){3.0f, 12.0f, 3.0f, 10.0f}
                               : (VkrUiEdges){3.0f, 9.0f, 3.0f, 9.0f};
    tab.style.corner_radius_pt = (Vec4){6.0f, 6.0f, 0.0f, 0.0f};
    tab.style.background_color = active ? theme->panel : (Vec4){0};
    tab.style.hover_background_color = active ? theme->panel : theme->row_hover;
    tab.style.text_color = active ? theme->text : theme->text_secondary;
    /* An accent underline marks the active tab; dock tabs mark theirs on
       top, so the two tiers read apart. */
    tab.style.border_pt =
        active ? (VkrUiEdges){0.0f, 0.0f, 2.0f, 0.0f} : (VkrUiEdges){0};
    tab.style.border_color = theme->accent;
    tab.icon = s_workbenches[item->kind].icon;
    tab.icon_size_pt = 14.0f;
    tab.icon_color = active ? theme->accent_hover : theme->text_secondary;
    /* A tab showing another scene than the active one names it too. */
    char other[96] = "";
    if (!active && item->scene_known && editor->projects) {
      const String8 shown = vkr_editor_projects_scene_id(editor->projects);
      if (shown.length != strlen(item->scene_id) ||
          MemCompare(shown.str, item->scene_id, shown.length) != 0) {
        const String8 name = item->scene_id[0]
                                 ? vkr_editor_projects_scene_name_of(
                                       editor->projects, item->scene_id)
                                 : string8_lit("World");
        snprintf(other, sizeof(other), "%.*s", (int)name.length,
                 (const char *)name.str);
      }
    }
    char tip[160];
    snprintf(tip, sizeof(tip), other[0] ? "%s: %s" : "%s%s", item->name, other);
    tab.tooltip = vkr_editor_command_tooltip(
        ui, (EditorCommand)(CMD_WORKBENCH_1 + i), tip);
    (void)vkr_ui_push_id_u64(ui, i);
    const VkrUiId id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("workbench"));
    /* A click switches, a second click within 0.4 s renames the tab and a
       right click opens its menu. */
    const char *label = !labels ? "" : active ? scene : other;
    if (vkr_ui_button(
            ui, string8_lit("workbench"),
            string8_create_from_cstr((const uint8_t *)label, strlen(label)),
            &tab)) {
      const float64_t now = vkr_platform_get_absolute_time();
      if (workbenches->click_tab == i && now - workbenches->click_time < 0.4) {
        vkr_editor_workbench_rename_begin(editor, i);
      } else if (!active) {
        char message[96];
        if (!vkr_editor_workbench_request(editor, frame, i, message,
                                          sizeof(message))) {
          vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, theme->warning,
                           message);
        }
      }
      workbenches->click_tab = i;
      workbenches->click_time = now;
    }
    if (ui->hot_id == id && !ui->mouse_captured &&
        input_button_just_pressed(frame->input, BUTTON_RIGHT)) {
      vkr_editor_context_open(
          editor, VKR_EDITOR_CONTEXT_WORKBENCH,
          (Vec2){(float32_t)ui->mouse_x / ui->content_scale,
                 (float32_t)ui->mouse_y / ui->content_scale});
      editor->context_panel = i;
    }
    (void)vkr_ui_pop_id(ui);
  }
  if (count < VKR_EDITOR_WORKBENCH_MAX) {
    VkrUiWidgetConfig add = vkr_editor_icon_button_config(
        count, 0u, VKR_UI_ICON_ADD,
        string8_lit("New workbench: a copy of this one"));
    add.placement.align = VKR_UI_ALIGN_CENTER;
    add.style.min_size_pt = add.style.max_size_pt = (Vec2){22.0f, 22.0f};
    add.style.padding_pt = (VkrUiEdges){4.0f, 4.0f, 4.0f, 4.0f};
    add.icon_size_pt = 12.0f;
    if (vkr_ui_button(ui, string8_lit("workbench.add"), (String8){0}, &add)) {
      char message[96];
      if (!vkr_editor_workbench_duplicate(editor, frame, workbenches->active,
                                          message, sizeof(message))) {
        vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, theme->warning,
                         message);
      }
    }
  }
  (void)vkr_ui_panel_end(ui);
}

// =============================================================================
// Palettes
// =============================================================================

#define PALETTE_PAD_PT 10.0f
#define PALETTE_ROW_PT 28.0f
#define PALETTE_GAP_PT 4.0f

VkrEditorPalette vkr_editor_palette_begin(VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame,
                                          VkrUiRect bounds, float32_t y) {
  const float32_t width = bounds.width / frame->ui->content_scale;
  return (VkrEditorPalette){
      .editor = editor,
      .frame = frame,
      .ui = frame->ui,
      .width = width,
      .y = y,
      .columns = width >= 200.0f ? 2u : 1u,
      .labels = width >= 110.0f,
  };
}

void vkr_editor_palette_end(VkrEditorPalette *palette) {
  if (palette->slot) {
    const uint32_t rows =
        (palette->slot + palette->columns - 1u) / palette->columns;
    palette->y += (float32_t)rows * (PALETTE_ROW_PT + PALETTE_GAP_PT);
    palette->slot = 0u;
  }
}

void vkr_editor_palette_heading(VkrEditorPalette *palette, String8 id,
                                String8 text) {
  vkr_editor_palette_end(palette);
  const VkrUiTheme *theme = vkr_ui_theme();
  palette->y += 6.0f;
  VkrUiWidgetConfig label =
      vkr_editor_details_widget(PALETTE_PAD_PT, palette->y,
                                palette->width - PALETTE_PAD_PT * 2.0f, 18.0f);
  label.style.font_size_pt = theme->font_caption;
  label.style.text_color = theme->text_secondary;
  label.text.font = palette->editor->heading_font;
  vkr_ui_label(palette->ui, id, text, &label);
  palette->y += 22.0f;
}

bool8_t vkr_editor_palette_button(VkrEditorPalette *palette, String8 id,
                                  const char *label, VkrUiIcon icon,
                                  String8 tooltip, bool8_t active,
                                  bool8_t disabled) {
  const float32_t inner = palette->width - PALETTE_PAD_PT * 2.0f;
  const float32_t cell =
      (inner - PALETTE_GAP_PT * (float32_t)(palette->columns - 1u)) /
      (float32_t)palette->columns;
  const uint32_t column = palette->slot % palette->columns;
  const uint32_t row = palette->slot / palette->columns;
  palette->slot++;
  VkrUiWidgetConfig button = vkr_editor_details_widget(
      PALETTE_PAD_PT + (cell + PALETTE_GAP_PT) * (float32_t)column,
      palette->y + (PALETTE_ROW_PT + PALETTE_GAP_PT) * (float32_t)row, cell,
      PALETTE_ROW_PT);
  vkr_editor_toggle_style(&button, active);
  button.icon = icon;
  button.icon_size_pt = 14.0f;
  const String8 text =
      string8_create_from_cstr((const uint8_t *)label, strlen(label));
  button.tooltip = tooltip.length ? tooltip : text;
  button.disabled = disabled;
  if (disabled) {
    button.style.text_color = vkr_ui_theme()->text_disabled;
    button.icon_color = vkr_ui_theme()->text_disabled;
  }
  return vkr_ui_button(palette->ui, id, palette->labels ? text : (String8){0},
                       &button) &&
         !disabled;
}

void vkr_editor_palette_command(VkrEditorPalette *palette, String8 id,
                                const char *label, VkrUiIcon icon,
                                EditorCommand command, bool8_t active) {
  const bool8_t enabled =
      vkr_editor_command_enabled(command, palette->editor, palette->frame);
  if (vkr_editor_palette_button(
          palette, id, label, icon,
          vkr_editor_command_tooltip(palette->ui, command, label), active,
          !enabled)) {
    vkr_editor_command_execute(command, palette->editor, palette->frame);
  }
}

void vkr_editor_palette_create(VkrEditorPalette *palette, String8 id,
                               const char *label, VkrUiIcon icon,
                               const char *word) {
  const uint16_t container = vkr_editor_create_container(palette->frame);
  if (!vkr_editor_palette_button(
          palette, id, label, icon, (String8){0}, false_v,
          container == UINT16_MAX || !palette->frame->mapping_valid)) {
    return;
  }
  for (uint32_t kind = 0; kind < vkr_editor_object_kind_count(); ++kind) {
    if (strcmp(vkr_editor_object_kind_word(kind), word) == 0) {
      (void)vkr_editor_request_create(palette->editor, palette->frame, kind,
                                      container, NULL);
      return;
    }
  }
}

/* Lights and environment objects, Bake lighting, the lighting view modes and
   the light icons. */
static void workbench_lighting_palette(VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       VkrUiRect bounds) {
  VkrEditorPalette palette =
      vkr_editor_palette_begin(editor, frame, bounds, 4.0f);
  vkr_editor_palette_heading(&palette, string8_lit("lighting.lights"),
                             string8_lit("LIGHTS"));
  vkr_editor_palette_create(&palette, string8_lit("lighting.point"), "Point",
                            VKR_UI_ICON_POINT_LIGHT, "point_light");
  vkr_editor_palette_create(&palette, string8_lit("lighting.spot"), "Spot",
                            VKR_UI_ICON_SPOT_LIGHT, "spot_light");
  vkr_editor_palette_create(&palette, string8_lit("lighting.rect"), "Rect",
                            VKR_UI_ICON_RECT_LIGHT, "rect_light");
  vkr_editor_palette_create(&palette, string8_lit("lighting.directional"),
                            "Directional", VKR_UI_ICON_DIRECTIONAL_LIGHT,
                            "directional_light");
  vkr_editor_palette_heading(&palette, string8_lit("lighting.environment"),
                             string8_lit("ENVIRONMENT"));
  vkr_editor_palette_create(&palette, string8_lit("lighting.sky"), "Sky",
                            VKR_UI_ICON_PLANET, "atmosphere");
  vkr_editor_palette_create(&palette, string8_lit("lighting.clouds"), "Clouds",
                            VKR_UI_ICON_CLOUD, "clouds");
  vkr_editor_palette_create(&palette, string8_lit("lighting.fog"), "Fog",
                            VKR_UI_ICON_FOG, "fog");
  vkr_editor_palette_create(&palette, string8_lit("lighting.volumetric"),
                            "Volumetric fog", VKR_UI_ICON_FOG,
                            "volumetric_fog");
  vkr_editor_palette_create(&palette, string8_lit("lighting.post"),
                            "Post process", VKR_UI_ICON_PALETTE,
                            "post_process");

  /* View modes set the render mode directly, as Cmd view.mode does. */
  vkr_editor_palette_heading(&palette, string8_lit("lighting.view"),
                             string8_lit("VIEW"));
  static const struct {
    const char *label;
    VkrUiIcon icon;
    VkrRenderMode mode;
  } modes[] = {
      {"Lit", VKR_UI_ICON_SUN_DIM, VKR_RENDER_MODE_DEFAULT},
      {"Lighting only", VKR_UI_ICON_LIGHT, VKR_RENDER_MODE_LIGHTING_ONLY},
      {"Detail lighting", VKR_UI_ICON_VIEW_MODE,
       VKR_RENDER_MODE_DETAIL_LIGHTING},
  };
  for (uint32_t i = 0u; i < ArrayCount(modes); ++i) {
    (void)vkr_ui_push_id_u64(frame->ui, i);
    if (vkr_editor_palette_button(&palette, string8_lit("lighting.mode"),
                                  modes[i].label, modes[i].icon, (String8){0},
                                  frame->view_state.render_mode ==
                                      modes[i].mode,
                                  !frame->view_request)) {
      VkrSampleViewState next = frame->view_state;
      next.render_mode = modes[i].mode;
      *frame->view_request =
          (VkrSampleViewRequest){.value = next, .apply = true_v};
    }
    (void)vkr_ui_pop_id(frame->ui);
  }
  vkr_editor_palette_command(&palette, string8_lit("lighting.icons"),
                             "Light icons", VKR_UI_ICON_EYE, CMD_LABELS,
                             editor->labels_enabled);

  vkr_editor_palette_heading(&palette, string8_lit("lighting.bake"),
                             string8_lit("BAKE"));
  vkr_editor_palette_command(&palette, string8_lit("lighting.bake_button"),
                             "Bake lighting", VKR_UI_ICON_BAKERY,
                             CMD_SCENE_BAKE, false_v);
  vkr_editor_palette_command(&palette, string8_lit("lighting.preferences"),
                             "Preferences", VKR_UI_ICON_GRAPHICS, CMD_GRAPHICS,
                             false_v);
  vkr_editor_palette_end(&palette);
}

void vkr_editor_workbench_tools_build(VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      VkrUiRect bounds) {
  const VkrEditorWorkbenchKind kind =
      editor->workbenches.items[editor->workbenches.active].kind;
  if (kind == VKR_EDITOR_WORKBENCH_LEVEL_DESIGN) {
    vkr_editor_level_palette_build(editor, frame, bounds);
    return;
  }
  if (kind == VKR_EDITOR_WORKBENCH_LIGHTING) {
    workbench_lighting_palette(editor, frame, bounds);
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig note = vkr_editor_details_widget(
      10.0f, 10.0f, Max(40.0f, bounds.width / ui->content_scale - 20.0f),
      40.0f);
  note.style.font_size_pt = theme->font_body;
  note.style.text_color = theme->text_secondary;
  note.text.layout.word_wrap = true_v;
  note.text.layout.max_width =
      Max(1.0f, bounds.width / ui->content_scale - 20.0f);
  vkr_ui_label(ui, string8_lit("tools.empty"),
               string8_lit("This workbench has no tools. Level Design "
                           "(Ctrl+2) and Lighting (Ctrl+4) have palettes."),
               &note);
}

// =============================================================================
// Persistence
// =============================================================================

static bool8_t workbench_name(VkrJsonWriter *writer, const char *name) {
  return vkr_json_writer_name(
      writer, string8_create_from_cstr((const uint8_t *)name, strlen(name)));
}

static bool8_t workbench_text(VkrJsonWriter *writer, const char *name,
                              const char *value) {
  return workbench_name(writer, name) &&
         vkr_json_writer_string(
             writer,
             string8_create_from_cstr((const uint8_t *)value, strlen(value)));
}

bool8_t vkr_editor_workbench_write_json(const VkrEditorUi *editor,
                                        const VkrUiDockTree *live,
                                        VkrJsonWriter *writer) {
  const VkrEditorWorkbenches *workbenches = &editor->workbenches;
  bool8_t ok =
      vkr_json_writer_begin_object(writer) &&
      workbench_name(writer, "version") && vkr_json_writer_u64(writer, 1u) &&
      workbench_text(writer, "active",
                     workbenches->items[workbenches->active].id) &&
      workbench_name(writer, "order") && vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0u; ok && i < workbenches->count; ++i) {
    const char *id = workbenches->items[i].id;
    ok = vkr_json_writer_string(
        writer, string8_create_from_cstr((const uint8_t *)id, strlen(id)));
  }
  ok = ok && vkr_json_writer_end_array(writer);
  for (uint32_t i = 0u; ok && i < workbenches->count; ++i) {
    const VkrEditorWorkbench *item = &workbenches->items[i];
    const bool8_t active = i == workbenches->active;
    const uint32_t windows =
        active ? workbench_open_windows(editor) : item->windows;
    const VkrEditorWorkbenchMode *mode = &item->mode;
    uint32_t tool = 0u;
    while (tool < 3u && vkr_editor_cmd_tool_modes[tool] != mode->gizmo_tool) {
      ++tool;
    }
    ok = workbench_name(writer, item->id) &&
         vkr_json_writer_begin_object(writer) &&
         workbench_text(writer, "kind", s_workbenches[item->kind].id) &&
         workbench_text(writer, "name", item->name) &&
         (!item->scene_known ||
          workbench_text(writer, "scene", item->scene_id)) &&
         workbench_name(writer, "layout") &&
         vkr_ui_dock_write_json(writer, active ? live : &item->layout) &&
         workbench_name(writer, "windows") &&
         vkr_json_writer_begin_object(writer);
    for (uint32_t w = 0u; ok && w < VKR_EDITOR_WINDOW_COUNT; ++w) {
      if ((windows >> w) & 1u) {
        ok = workbench_name(
                 writer, vkr_editor_cmd_window_name((VkrEditorWindowKind)w)) &&
             vkr_json_writer_bool(writer, true_v);
      }
    }
    ok =
        ok && vkr_json_writer_end_object(writer) &&
        workbench_name(writer, "mode") &&
        vkr_json_writer_begin_object(writer) &&
        workbench_text(writer, "tool", vkr_editor_cmd_tools[tool]) &&
        workbench_text(writer, "scene_tool", s_scene_tools[mode->scene_tool]) &&
        workbench_text(writer, "snap", s_snap_targets[mode->snap]) &&
        workbench_name(writer, "grid") &&
        vkr_json_writer_bool(writer, mode->grid) &&
        vkr_json_writer_end_object(writer) &&
        vkr_json_writer_end_object(writer);
  }
  return ok && vkr_json_writer_end_object(writer);
}

/* The object or array `name` of the object `json`, as a view. */
static String8 workbench_member(String8 json, const char *name) {
  VkrJsonReader reader = vkr_json_reader_from_string(json);
  String8 value = {0};
  if (!vkr_json_find_root_field(&reader, name) ||
      !vkr_json_capture_composite(&reader, &value)) {
    return (String8){0};
  }
  return value;
}

/* The index of `value` among `count` names, or `fallback`. */
static uint32_t workbench_choice(String8 json, const char *name,
                                 const char *const *names, uint32_t count,
                                 uint32_t fallback) {
  VkrJsonReader reader = vkr_json_reader_from_string(json);
  String8 value = {0};
  if (!vkr_json_find_root_field(&reader, name) ||
      !vkr_json_parse_string(&reader, &value)) {
    return fallback;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    if (workbench_word_is(value, names[i])) {
      return i;
    }
  }
  return fallback;
}

static void workbench_read_item(String8 json, VkrEditorWorkbench *item) {
  VkrJsonReader scene_reader = vkr_json_reader_from_string(json);
  String8 scene = {0};
  if (vkr_json_find_root_field(&scene_reader, "scene") &&
      vkr_json_parse_string(&scene_reader, &scene) &&
      scene.length < sizeof(item->scene_id)) {
    snprintf(item->scene_id, sizeof(item->scene_id), "%.*s", (int)scene.length,
             scene.str);
    item->scene_known = true_v;
  }
  VkrJsonReader name_reader = vkr_json_reader_from_string(json);
  String8 name = {0};
  if (vkr_json_find_root_field(&name_reader, "name") &&
      vkr_json_parse_string(&name_reader, &name) && name.length &&
      name.length < sizeof(item->name)) {
    snprintf(item->name, sizeof(item->name), "%.*s", (int)name.length,
             name.str);
  }
  VkrUiDockTree tree;
  const String8 layout = workbench_member(json, "layout");
  if (layout.length && vkr_ui_dock_read_json(layout, &tree)) {
    tree.toolbar_pt = workbench_chrome_pt();
    item->layout = tree;
  }
  const String8 windows = workbench_member(json, "windows");
  item->windows = 0u;
  for (uint32_t w = 0u; windows.length && w < VKR_EDITOR_WINDOW_COUNT; ++w) {
    VkrJsonReader reader = vkr_json_reader_from_string(windows);
    bool8_t open = false_v;
    if (vkr_json_find_root_field(
            &reader, vkr_editor_cmd_window_name((VkrEditorWindowKind)w)) &&
        vkr_json_parse_bool(&reader, &open) && open) {
      item->windows |= 1u << w;
    }
  }
  const String8 mode = workbench_member(json, "mode");
  if (mode.length) {
    VkrEditorWorkbenchMode *out = &item->mode;
    const uint32_t tool =
        workbench_choice(mode, "tool", vkr_editor_cmd_tools, 4u, UINT32_MAX);
    if (tool != UINT32_MAX) {
      out->gizmo_tool = vkr_editor_cmd_tool_modes[tool];
    }
    out->scene_tool = (VkrEditorSceneTool)workbench_choice(
        mode, "scene_tool", s_scene_tools, VKR_EDITOR_SCENE_TOOL_COUNT,
        out->scene_tool);
    out->snap = (VkrEditorSnapTarget)workbench_choice(
        mode, "snap", s_snap_targets, VKR_EDITOR_SNAP_COUNT, out->snap);
    VkrJsonReader reader = vkr_json_reader_from_string(mode);
    bool8_t grid = out->grid;
    if (vkr_json_find_root_field(&reader, "grid") &&
        vkr_json_parse_bool(&reader, &grid)) {
      out->grid = grid;
    }
  }
}

/* The next string of a JSON array of strings at `*at`; false at its end. */
static bool8_t workbench_next_string(String8 array, uint64_t *at,
                                     String8 *out) {
  uint64_t start = *at;
  while (start < array.length && array.str[start] != '"') {
    if (array.str[start] == ']') {
      return false_v;
    }
    ++start;
  }
  uint64_t end = start + 1u;
  while (end < array.length && array.str[end] != '"') {
    ++end;
  }
  if (end >= array.length) {
    return false_v;
  }
  *out = string8_create(array.str + start + 1u, end - start - 1u);
  *at = end + 1u;
  return true_v;
}

/* Builds the list in the saved order: built-ins by id, copies by their
   kind; built-ins the order leaves out follow. */
static void workbench_read_order(VkrEditorWorkbenches *workbenches,
                                 String8 json) {
  const String8 order = workbench_member(json, "order");
  if (!order.length) {
    return;
  }
  const VkrEditorWorkbenches builtins = *workbenches;
  bool8_t placed[VKR_EDITOR_WORKBENCH_COUNT] = {0};
  uint32_t count = 0u;
  uint64_t at = 0u;
  String8 id = {0};
  const char *kinds[VKR_EDITOR_WORKBENCH_COUNT];
  for (uint32_t i = 0u; i < VKR_EDITOR_WORKBENCH_COUNT; ++i) {
    kinds[i] = s_workbenches[i].id;
  }
  while (count < VKR_EDITOR_WORKBENCH_MAX &&
         workbench_next_string(order, &at, &id)) {
    if (!id.length || id.length >= VKR_EDITOR_WORKBENCH_ID_CAPACITY) {
      continue;
    }
    char key[VKR_EDITOR_WORKBENCH_ID_CAPACITY];
    snprintf(key, sizeof(key), "%.*s", (int)id.length, id.str);
    uint32_t builtin = UINT32_MAX;
    for (uint32_t i = 0u; i < VKR_EDITOR_WORKBENCH_COUNT; ++i) {
      if (strcmp(key, s_workbenches[i].id) == 0) {
        builtin = i;
      }
    }
    if (builtin != UINT32_MAX) {
      if (!placed[builtin]) {
        placed[builtin] = true_v;
        workbenches->items[count++] = builtins.items[builtin];
      }
      continue;
    }
    /* A copy names the built-in it copies. */
    const String8 member = workbench_member(json, key);
    const uint32_t kind =
        member.length ? workbench_choice(member, "kind", kinds,
                                         VKR_EDITOR_WORKBENCH_COUNT, UINT32_MAX)
                      : UINT32_MAX;
    if (kind == UINT32_MAX) {
      continue;
    }
    VkrEditorWorkbench *item = &workbenches->items[count++];
    *item = builtins.items[kind];
    item->custom = true_v;
    snprintf(item->id, sizeof(item->id), "%s", key);
    uint32_t number = 0u;
    if (sscanf(key, "custom_%u", &number) == 1 &&
        number >= workbenches->next_custom) {
      workbenches->next_custom = number + 1u;
    }
  }
  for (uint32_t i = 0u; i < VKR_EDITOR_WORKBENCH_COUNT; ++i) {
    if (!placed[i] && count < VKR_EDITOR_WORKBENCH_MAX) {
      workbenches->items[count++] = builtins.items[i];
    }
  }
  workbenches->count = count;
}

bool8_t vkr_editor_workbench_read_json(VkrEditorUi *editor, String8 json,
                                       VkrUiDockTree *live) {
  VkrJsonReader reader = vkr_json_reader_from_string(json);
  String8 active_id = {0};
  if (!vkr_json_find_root_field(&reader, "active") ||
      !vkr_json_parse_string(&reader, &active_id)) {
    return false_v;
  }
  VkrEditorWorkbenches *workbenches = &editor->workbenches;
  vkr_editor_workbench_init(workbenches);
  workbench_read_order(workbenches, json);
  uint32_t active = 0u;
  for (uint32_t i = 0u; i < workbenches->count; ++i) {
    VkrEditorWorkbench *item = &workbenches->items[i];
    if (workbench_word_is(active_id, item->id)) {
      active = i;
    }
    const String8 member = workbench_member(json, item->id);
    if (member.length) {
      workbench_read_item(member, item);
    }
  }
  workbenches->active = active;
  *live = workbenches->items[active].layout;
  /* Project settings restore window geometry and visibility after this;
     the layout file has only these. */
  for (uint32_t i = 0u; i < VKR_EDITOR_WINDOW_COUNT; ++i) {
    editor->windows[i].visible = (workbenches->items[active].windows >> i) & 1u;
  }
  return true_v;
}

bool8_t vkr_editor_workbench_load_file(VkrEditorUi *editor, VkrUiDockTree *live,
                                       String8 path) {
  if (!path.str || path.length == 0u ||
      path.length > VKR_JSON_WRITER_PATH_MAX ||
      memchr(path.str, '\0', path.length)) {
    return false_v;
  }
  char cpath[VKR_JSON_WRITER_PATH_MAX + 1u];
  MemCopy(cpath, path.str, path.length);
  cpath[path.length] = '\0';
  FILE *file = file_fopen(cpath, "rb");
  if (!file) {
    return false_v;
  }
  /* Read once at startup; five full trees fit. */
  static uint8_t json[KB(64)];
  const size_t length = fread(json, 1u, sizeof(json), file);
  const bool8_t complete = !ferror(file) && feof(file);
  fclose(file);
  if (!complete || length == 0u) {
    return false_v;
  }
  const String8 text = string8_create(json, length);
  if (vkr_editor_workbench_read_json(editor, text, live)) {
    return true_v;
  }
  /* A bare dock tree is General's layout. */
  VkrUiDockTree tree;
  if (!vkr_ui_dock_read_json(text, &tree)) {
    return false_v;
  }
  vkr_editor_workbench_init(&editor->workbenches);
  tree.toolbar_pt = workbench_chrome_pt();
  editor->workbenches.items[VKR_EDITOR_WORKBENCH_GENERAL].layout = tree;
  *live = tree;
  return true_v;
}

bool8_t vkr_editor_workbench_save_file(const VkrEditorUi *editor,
                                       const VkrUiDockTree *live,
                                       String8 path) {
  VkrJsonFileWriter file = {0};
  if (!vkr_json_file_writer_begin(&file, path)) {
    return false_v;
  }
  if (vkr_editor_workbench_write_json(editor, live, &file.writer) &&
      vkr_json_file_writer_commit(&file)) {
    return true_v;
  }
  vkr_json_file_writer_abort(&file);
  return false_v;
}
