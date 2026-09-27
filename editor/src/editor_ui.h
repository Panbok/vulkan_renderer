#pragma once

#include "editor_animation.h"
#include "editor_bakery.h"
#include "editor_console.h"
#include "editor_content.h"
#include "editor_details.h"
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
  /* Create a scene or import an asset (ADR-076). */
  VKR_EDITOR_WINDOW_CREATE,
  VKR_EDITOR_WINDOW_COUNT,
} VkrEditorWindowKind;

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
  /* Container that owns the entity, valid for the UI frame. */
  const struct VkrScene *scene;
  /* Place in the column of abstract objects at the world origin, or
     UINT32_MAX to follow the entity's position. */
  uint32_t stack;
} VkrEditorLabelAnchor;

typedef struct VkrEditorPhysicsLine VkrEditorPhysicsLine;

#define VKR_EDITOR_GRID_LINE_CAPACITY 192u

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
  /* Line opacity: minor perspective lines fade as the camera rises. */
  float32_t alpha;
  /* Minor perspective lines are never labelled. */
  bool8_t unlabelled;
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
  /* One descriptor-typed component of an entity. */
  VKR_EDITOR_CMD_VALUE_COMPONENT,
  VKR_EDITOR_CMD_VALUE_OBJECT,
} VkrEditorCmdValueKind;

typedef struct VkrEditorCmdValue {
  VkrEditorCmdValueKind kind;
  float64_t number;
  Vec3 vector;
  VkrEntityId entity;
  const VkrTypeDesc *type; /* COMPONENT only. */
  uint32_t object;
  char text[96];
} VkrEditorCmdValue;

typedef struct VkrEditorCmdVariable {
  char name[32];
  VkrEditorCmdValue value;
} VkrEditorCmdVariable;

/* Viewport documents (ADR-076): tabs beside the Scene panel's tab, each
   showing the World alone or one project scene. Switching loads the tab's
   scene; only the active document is loaded and rendered. */
#define VKR_EDITOR_VIEWPORT_TAB_MAX 6u

typedef struct VkrEditorViewportTab {
  char scene_id[37];
  char label[64];
} VkrEditorViewportTab;

/* What a right-click menu acts on; each kind has its own item table. */
typedef enum VkrEditorContextKind {
  VKR_EDITOR_CONTEXT_ENTITY = 0,
  VKR_EDITOR_CONTEXT_DOCK_TAB,
  VKR_EDITOR_CONTEXT_CONSOLE,
  /* Object creation from the Outliner's add button (ADR-076). */
  VKR_EDITOR_CONTEXT_CREATE,
  /* Component types the Details panel can add to the context entity. */
  VKR_EDITOR_CONTEXT_ADD_COMPONENT,
  /* Save the context entity's `context_type` component as a preset, or apply
     one of that type (ADR-076). */
  VKR_EDITOR_CONTEXT_PRESET,
  /* Open, Put into viewport, Rename and Delete for a Content item. */
  VKR_EDITOR_CONTEXT_CONTENT,
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
  /* World id a creation menu adds to. */
  uint16_t context_container;
  /* Component type a preset menu acts on. */
  const VkrTypeDesc *context_type;
  VkrEditorViewportTab viewport_tabs[VKR_EDITOR_VIEWPORT_TAB_MAX];
  uint32_t viewport_tab_count;
  uint32_t viewport_tab_active;
  /* Rows the open menu showed last build; sizes its input region. */
  uint32_t context_count;
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
  /* Preferences window rows and scroll offset. */
  VkrEditorDetails preferences_details;
  float32_t preferences_scroll;
  float32_t preferences_height;
  VkrEditorAnimation animation;
  /* Additive scene path and sidecar a Cmd request borrows until the
     runtime consumes it (ADR-076). */
  char cmd_scene_path[1024];
  char cmd_scene_sidecar[1024];

  VkrEditorWindowState windows[VKR_EDITOR_WINDOW_COUNT];
} VkrEditorUi;

void vkr_editor_ui_init(VkrEditorUi *editor);
VkrUiDockInputCapture vkr_editor_ui_build(VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame);
