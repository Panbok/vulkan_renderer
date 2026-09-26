#pragma once

#include "editor_animation.h"
#include "editor_bakery.h"
#include "editor_console.h"
#include "editor_content.h"
#include "editor_physics_settings.h"
#include "editor_scene_panels.h"
#include "vkr_sample_runtime.h"

typedef struct VkrEditorProjects VkrEditorProjects;

typedef enum VkrEditorMenu {
  VKR_EDITOR_MENU_NONE = 0,
  VKR_EDITOR_MENU_FILE,
  VKR_EDITOR_MENU_EDIT,
  VKR_EDITOR_MENU_VIEW,
  VKR_EDITOR_MENU_SCENE,
  VKR_EDITOR_MENU_HELP,
  VKR_EDITOR_MENU_COUNT,
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

/* Cmd evaluator value; objects name editor data roots (view, ui, sim, scene)
 * and lights address an entity's light component. */
typedef enum VkrEditorCmdValueKind {
  VKR_EDITOR_CMD_VALUE_NONE = 0,
  VKR_EDITOR_CMD_VALUE_NUMBER,
  VKR_EDITOR_CMD_VALUE_BOOL,
  VKR_EDITOR_CMD_VALUE_STRING,
  VKR_EDITOR_CMD_VALUE_VEC3,
  VKR_EDITOR_CMD_VALUE_ENTITY,
  VKR_EDITOR_CMD_VALUE_LIGHT,
  VKR_EDITOR_CMD_VALUE_OBJECT,
} VkrEditorCmdValueKind;

typedef struct VkrEditorCmdValue {
  VkrEditorCmdValueKind kind;
  float64_t number;
  Vec3 vector;
  VkrEntityId entity;
  uint32_t object;
  char text[96];
} VkrEditorCmdValue;

typedef struct VkrEditorCmdVariable {
  char name[32];
  VkrEditorCmdValue value;
} VkrEditorCmdVariable;

/* What a right-click menu acts on; each kind has its own item table. */
typedef enum VkrEditorContextKind {
  VKR_EDITOR_CONTEXT_ENTITY = 0,
  VKR_EDITOR_CONTEXT_DOCK_TAB,
  VKR_EDITOR_CONTEXT_CONSOLE,
} VkrEditorContextKind;

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
  /* Transient notification shown at the bottom of the window. */
  char toast_text[160];
  VkrUiIcon toast_icon;
  Vec4 toast_color;
  float64_t toast_seconds;
  uint64_t toast_saved_revision;
  bool8_t toast_saved_known;
  uint64_t toast_scene_generation;
  bool8_t toast_bakery_busy;
  /* Right-click menu on a Hierarchy row, anchored at the press in points. */
  bool8_t context_open;
  VkrEditorContextKind context_kind;
  Vec2 context_position_pt;
  VkrEntityId context_entity;
  /* VkrUiDockPanelKind of the tab a dock-tab menu acts on. */
  uint32_t context_panel;
  /* Mirrors of the UI system's interface zoom and reduced-motion setting,
   * kept for workspace persistence. */
  float32_t ui_scale;
  bool8_t reduce_motion;
  /* Leading top-bar space reserved for native window controls. */
  float32_t title_inset_pt;
  /* Pixel rectangle of the menu-bar button that opened `menu`. */
  VkrUiRect menu_anchor_px;
  bool8_t labels_enabled;
  bool8_t labels_directional;
  bool8_t labels_spot;
  bool8_t labels_point;
  /* Frame-scratch records, consumed before UI geometry preparation. */
  VkrEditorLabelAnchor *label_anchors;
  uint32_t label_anchor_count;
  uint64_t label_scene_generation;
  VkrUiId label_panel;
  bool8_t label_capacity_warned;
  /* Cmd bar. `cmd_active` mirrors field focus for other input owners;
   * suggestions are recomputed only when the text changes. */
  uint8_t cmd_text[256];
  uint32_t cmd_length;
  VkrUiId cmd_field;
  bool8_t cmd_active;
  bool8_t cmd_focus_request;
  bool8_t cmd_dirty;
  int32_t cmd_selected;
  uint32_t cmd_suggestion_count;
  char cmd_suggestions[8][96];
  char cmd_suggestion_hints[8][96];
  VkrUiRect cmd_popup_px;
  char cmd_history[16][256];
  uint32_t cmd_history_count;
  uint32_t cmd_history_cursor;
  /* Evaluator variables, kept for the editor session. */
  VkrEditorCmdVariable cmd_variables[32];
  uint32_t cmd_variable_count;
  /* Script queue: pending text, read offset and the active wait. */
  char cmd_queue[4096];
  uint32_t cmd_queue_length;
  uint32_t cmd_queue_offset;
  float64_t cmd_wait_seconds;
  float64_t cmd_wait_scene_seconds;
  VkrFontHandle heading_font;
  /* Inter body text, Phosphor icon atlases and the monospace Console face. */
  VkrFontHandle text_font;
  VkrFontHandle mono_font;
  VkrFontHandle icon_font;
  VkrFontHandle icon_fill_font;
  /* Pinned Scene header: left chip group rectangle, open dropdown, its
   * anchor chip and popup rectangle, all in points. */
  Vec4 view_toolbar_rect_pt;
  Vec4 view_popup_anchor_pt;
  Vec4 view_popup_rect_pt;
  uint32_t view_popup;
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
