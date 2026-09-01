/**
 * @file vkr_ui_dock.h
 * @brief In-window binary dock tree, tab drag/drop, and JSON persistence.
 */
#pragma once

#include "core/input.h"
#include "core/ui/vkr_ui_types.h"
#include "core/vkr_json_writer.h"

#define VKR_UI_DOCK_NODE_CAPACITY 31u
#define VKR_UI_DOCK_TAB_CAPACITY 8u
#define VKR_UI_DOCK_NODE_NONE UINT32_MAX
#define VKR_UI_DOCK_JSON_CAPACITY KB(16)

typedef enum VkrUiDockNodeKind {
  VKR_UI_DOCK_NODE_SPLIT = 0,
  VKR_UI_DOCK_NODE_TABS,
} VkrUiDockNodeKind;

typedef enum VkrUiDockSplitAxis {
  VKR_UI_DOCK_SPLIT_X = 0,
  VKR_UI_DOCK_SPLIT_Y,
} VkrUiDockSplitAxis;

typedef enum VkrUiDockPanelKind {
  VKR_UI_DOCK_PANEL_SCENE_VIEWPORT = 0,
  VKR_UI_DOCK_PANEL_HIERARCHY,
  VKR_UI_DOCK_PANEL_INSPECTOR,
  VKR_UI_DOCK_PANEL_CONSOLE,
  VKR_UI_DOCK_PANEL_TOOLBAR,
  VKR_UI_DOCK_PANEL_CUSTOM,
  VKR_UI_DOCK_PANEL_COUNT,
} VkrUiDockPanelKind;

typedef enum VkrUiDockDropZone {
  VKR_UI_DOCK_DROP_CENTER = 0,
  VKR_UI_DOCK_DROP_LEFT,
  VKR_UI_DOCK_DROP_RIGHT,
  VKR_UI_DOCK_DROP_TOP,
  VKR_UI_DOCK_DROP_BOTTOM,
} VkrUiDockDropZone;

typedef struct VkrUiDockTab {
  uint64_t id;
  VkrUiDockPanelKind panel_kind;
} VkrUiDockTab;

typedef struct VkrUiDockNode {
  VkrUiDockNodeKind kind;
  uint32_t parent;
  VkrUiRect rect_px;
  bool8_t used;
  union {
    struct {
      VkrUiDockSplitAxis axis;
      float32_t ratio;
      uint32_t first;
      uint32_t second;
    } split;
    struct {
      VkrUiDockTab tabs[VKR_UI_DOCK_TAB_CAPACITY];
      uint32_t tab_count;
      uint32_t active_tab;
    } leaf;
  } as;
} VkrUiDockNode;

typedef struct VkrUiDockInteraction {
  uint32_t tab_leaf;
  uint32_t tab_index;
  uint32_t resize_split;
  int32_t press_x;
  int32_t press_y;
  bool8_t dragging_tab;
} VkrUiDockInteraction;

typedef struct VkrUiDockTree {
  VkrUiDockNode nodes[VKR_UI_DOCK_NODE_CAPACITY];
  uint32_t node_high_water;
  uint32_t root;
  uint64_t revision;
  float32_t splitter_px;
  float32_t tab_bar_px;
  VkrUiDockInteraction interaction;
} VkrUiDockTree;

typedef struct VkrUiDockInputCapture {
  bool8_t mouse;
  bool8_t dragging_tab;
  bool8_t resizing_split;
} VkrUiDockInputCapture;

void vkr_ui_dock_default_editor_layout(VkrUiDockTree *tree);
bool8_t vkr_ui_dock_validate(const VkrUiDockTree *tree);
bool8_t vkr_ui_dock_layout(VkrUiDockTree *tree, VkrUiRect target_rect_px,
                           float32_t splitter_px, float32_t tab_bar_px);
bool8_t vkr_ui_dock_find_panel(const VkrUiDockTree *tree,
                               VkrUiDockPanelKind panel_kind,
                               uint32_t *out_leaf, VkrUiRect *out_content_rect);
String8 vkr_ui_dock_panel_label(VkrUiDockPanelKind panel_kind);

bool8_t vkr_ui_dock_set_split_ratio(VkrUiDockTree *tree, uint32_t split,
                                    float32_t ratio);
bool8_t vkr_ui_dock_move_tab(VkrUiDockTree *tree, uint32_t source_leaf,
                             uint32_t source_tab, uint32_t target_leaf,
                             uint32_t target_index,
                             VkrUiDockDropZone drop_zone);
VkrUiDockInputCapture vkr_ui_dock_update_input(VkrUiDockTree *tree,
                                               const InputState *input,
                                               bool8_t mouse_captured);

bool8_t vkr_ui_dock_write_json(VkrJsonWriter *writer,
                               const VkrUiDockTree *tree);
bool8_t vkr_ui_dock_read_json(String8 json, VkrUiDockTree *out_tree);
bool8_t vkr_ui_dock_save_file(const VkrUiDockTree *tree, String8 path);
bool8_t vkr_ui_dock_load_file(VkrUiDockTree *tree, String8 path);
