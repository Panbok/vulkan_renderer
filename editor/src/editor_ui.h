#pragma once

#include "editor_bakery.h"
#include "editor_console.h"
#include "editor_content.h"
#include "editor_scene_panels.h"
#include "vkr_sample_runtime.h"

typedef struct VkrEditorProjects VkrEditorProjects;

typedef enum VkrEditorMenu {
  VKR_EDITOR_MENU_NONE = 0,
  VKR_EDITOR_MENU_SETTINGS,
  VKR_EDITOR_MENU_METRICS,
  VKR_EDITOR_MENU_DEBUG,
} VkrEditorMenu;

typedef enum VkrEditorWindowKind {
  VKR_EDITOR_WINDOW_GRAPHICS = 0,
  VKR_EDITOR_WINDOW_DRAWS,
  VKR_EDITOR_WINDOW_MEMORY,
  VKR_EDITOR_WINDOW_HELP,
  VKR_EDITOR_WINDOW_COUNT,
} VkrEditorWindowKind;

typedef enum VkrEditorGraphicsTab {
  VKR_EDITOR_GRAPHICS_TAB_DISPLAY = 0,
  VKR_EDITOR_GRAPHICS_TAB_QUALITY,
  VKR_EDITOR_GRAPHICS_TAB_LIGHTING,
  VKR_EDITOR_GRAPHICS_TAB_EFFECTS,
  VKR_EDITOR_GRAPHICS_TAB_COLOR,
  VKR_EDITOR_GRAPHICS_TAB_COUNT,
} VkrEditorGraphicsTab;

typedef struct VkrEditorWindowState {
  Vec2 position_pt;
  Vec2 size_pt;
  uint32_t z_order;
  bool8_t visible;
} VkrEditorWindowState;

typedef struct VkrEditorLabelAnchor {
  VkrUiId widget;
  VkrEntityId entity;
} VkrEditorLabelAnchor;

typedef struct VkrEditorUi {
  VkrEditorProjects *projects;
  VkrEditorConsole console;
  VkrEditorBakery *bakery;
  VkrEditorContent *content;
  VkrEditorScenePanels *scene_panels;
  VkrEditorMenu menu;
  bool8_t labels_expanded;
  bool8_t labels_enabled;
  bool8_t labels_directional;
  bool8_t labels_spot;
  bool8_t labels_point;
  VkrFontHandle label_font;
  /* Frame-scratch records, consumed before UI geometry preparation. */
  VkrEditorLabelAnchor *label_anchors;
  uint32_t label_anchor_count;
  uint64_t label_scene_generation;
  VkrUiId label_panel;
  bool8_t label_capacity_warned;
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
  VkrEditorGraphicsTab graphics_tab;

  VkrEditorWindowState windows[VKR_EDITOR_WINDOW_COUNT];
} VkrEditorUi;

void vkr_editor_ui_init(VkrEditorUi *editor);
VkrUiDockInputCapture vkr_editor_ui_build(VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame);
