#pragma once

#include "editor_bakery.h"
#include "editor_console.h"
#include "editor_scene_panels.h"
#include "vkr_sample_runtime.h"

typedef enum VkrEditorMenu {
  VKR_EDITOR_MENU_NONE = 0,
  VKR_EDITOR_MENU_METRICS,
} VkrEditorMenu;

typedef enum VkrEditorWindowKind {
  VKR_EDITOR_WINDOW_DRAWS = 0,
  VKR_EDITOR_WINDOW_MEMORY,
  VKR_EDITOR_WINDOW_HELP,
  VKR_EDITOR_WINDOW_COUNT,
} VkrEditorWindowKind;

typedef struct VkrEditorWindowState {
  Vec2 position_pt;
  Vec2 size_pt;
  uint32_t z_order;
  bool8_t visible;
} VkrEditorWindowState;

typedef struct VkrEditorUi {
  VkrEditorConsole console;
  VkrEditorBakery *bakery;
  VkrEditorScenePanels *scene_panels;
  VkrEditorMenu menu;
  bool8_t commands_open;
  bool8_t commands_focus_search;
  uint32_t commands_cursor;
  uint32_t commands_first_row;
  int32_t commands_repeat_direction;
  float64_t commands_repeat_remaining;
  uint8_t commands_query[96];
  uint32_t commands_query_length;
  VkrFontHandle heading_font;
  Vec2 toolbar_offset_pt;
  Vec2 toolbar_grab_pt;
  Vec4 toolbar_rect_pt;
  uint32_t toolbar_columns;
  int8_t toolbar_anchor_x;
  int8_t toolbar_anchor_y;
  bool8_t toolbar_initialized;
  bool8_t toolbar_dragging;

  VkrEditorWindowState windows[VKR_EDITOR_WINDOW_COUNT];
} VkrEditorUi;

void vkr_editor_ui_init(VkrEditorUi *editor);
VkrUiDockInputCapture vkr_editor_ui_build(VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame);
