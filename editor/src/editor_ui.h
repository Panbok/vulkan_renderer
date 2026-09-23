#pragma once

#include "editor_animation.h"
#include "editor_bakery.h"
#include "editor_console.h"
#include "editor_content.h"
#include "editor_scene_panels.h"
#include "editor_physics_settings.h"
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
  VKR_EDITOR_WINDOW_ANIMATION,
  VKR_EDITOR_WINDOW_PHYSICS,
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
  bool8_t dragging;
  Vec2 drag_grab_pt;
} VkrEditorWindowState;

typedef struct VkrEditorLabelAnchor {
  VkrUiId widget;
  VkrEntityId entity;
} VkrEditorLabelAnchor;

typedef struct VkrEditorPhysicsLine VkrEditorPhysicsLine;

#define VKR_EDITOR_GRID_LINE_CAPACITY 96u

typedef struct VkrEditorGridLine {
  VkrUiId widget;
  VkrUiId label;
  Vec3 from;
  Vec3 to;
  Vec3 label_offset;
  Vec2 label_size_pt;
  /* 1-based screen-order label: numbers left to right on top, letters top to
   * bottom on the right. Zero leaves the cell unlabeled. */
  uint32_t ordinal;
  bool8_t top_label;
  bool8_t world_axis;
} VkrEditorGridLine;

typedef struct VkrEditorUi {
  VkrEditorPhysicsLine *physics_lines;
  uint32_t physics_line_count;
  uint64_t physics_scene_generation;
  VkrUiId physics_panel;
  bool8_t physics_lines_truncated;
  VkrEditorProjects *projects;
  VkrEditorConsole console;
  VkrEditorBakery *bakery;
  VkrEditorContent *content;
  VkrEditorScenePanels *scene_panels;
  VkrEditorPhysicsSettings *physics_settings;
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
  Vec2 view_toolbar_offset_pt;
  Vec2 view_toolbar_grab_pt;
  Vec4 view_toolbar_rect_pt;
  Vec4 view_popup_rect_pt;
  uint32_t view_popup;
  bool8_t view_toolbar_initialized;
  bool8_t view_toolbar_dragging;
  bool8_t view_toolbar_overflow;
  VkrUiId grid_panel;
  uint64_t grid_frame;
  VkrSampleCameraView grid_camera_view;
  /* Right-label column width and top-label row height; each axis keeps out of
   * the other's strip so corner labels never collide. */
  Vec2 grid_reserved_pt;
  float32_t grid_spacing; /* Drawn world cell size; zero without a grid. */
  uint32_t grid_line_count;
  VkrEditorGridLine grid_lines[VKR_EDITOR_GRID_LINE_CAPACITY];
  VkrEditorGraphicsTab graphics_tab;
  VkrEditorAnimation animation;

  VkrEditorWindowState windows[VKR_EDITOR_WINDOW_COUNT];
} VkrEditorUi;

void vkr_editor_ui_init(VkrEditorUi *editor);
VkrUiDockInputCapture vkr_editor_ui_build(VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame);
