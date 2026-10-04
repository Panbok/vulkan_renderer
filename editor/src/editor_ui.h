#pragma once

#include "editor_animation.h"
#include "editor_bakery.h"
#include "editor_build.h"
#include "editor_code.h"
#include "editor_console.h"
#include "editor_content.h"
#include "editor_details.h"
#include "editor_physics_settings.h"
#include "editor_scene_panels.h"
#include "editor_scripts.h"
#include "vkr_sample_runtime.h"

typedef struct VkrEditorProjects VkrEditorProjects;

typedef enum VkrEditorMenu {
  VKR_EDITOR_MENU_NONE = 0,
  VKR_EDITOR_MENU_FILE,
  VKR_EDITOR_MENU_EDIT,
  VKR_EDITOR_MENU_VIEW,
  VKR_EDITOR_MENU_SCENE,
  /* Build the game (docs/proposals/project-packaging.md). */
  VKR_EDITOR_MENU_BUILD,
  /* Developer tools: Bakery recipes and renderer diagnostics. */
  VKR_EDITOR_MENU_DEVELOP,
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
  /* Game settings and build profiles. */
  VKR_EDITOR_WINDOW_BUILD,
  /* Project script sources (ADR-079). */
  VKR_EDITOR_WINDOW_SCRIPT,
  /* Agent changes awaiting review (docs/proposals/level-design-toolkit.md). */
  VKR_EDITOR_WINDOW_CHANGES,
  /* Level checks against the player capsule. */
  VKR_EDITOR_WINDOW_LEVEL,
  /* Terrain sculpting and painting. */
  VKR_EDITOR_WINDOW_TERRAIN,
  VKR_EDITOR_WINDOW_COUNT,
} VkrEditorWindowKind;

typedef struct VkrEditorWindowState {
  Vec2 position_pt;
  Vec2 size_pt;
  uint32_t z_order;
  bool8_t visible;
  bool8_t dragging;
  Vec2 drag_grab_pt;
  /* Windows with a corner grip: the grip is held. */
  bool8_t resizable;
  bool8_t resizing;
} VkrEditorWindowState;

/* Empty objects one container shows icons for, cached until its structure
   changes. */
#define VKR_EDITOR_LABEL_EMPTY_MAX 256u
typedef struct VkrEditorLabelEmpties {
  const struct VkrScene *scene;
  uint64_t generation;
  uint64_t revision;
  uint32_t count;
  VkrEntityId entities[VKR_EDITOR_LABEL_EMPTY_MAX];
} VkrEditorLabelEmpties;

typedef struct VkrEditorLabelAnchor {
  VkrUiId widget;
  VkrEntityId entity;
  /* Container that owns the entity, valid for the UI frame. */
  const struct VkrScene *scene;
  /* Place in the column of abstract objects at the world origin, or
     UINT32_MAX to follow the entity's position. */
  uint32_t stack;
  /* Entity-local point a placed icon marks: the origin, or the top center of
     a text's glyphs. */
  Vec3 pivot;
} VkrEditorLabelAnchor;

typedef struct VkrEditorPhysicsLine VkrEditorPhysicsLine;

#define VKR_EDITOR_GRID_LINE_CAPACITY 192u

typedef struct VkrEditorGridLine {
  VkrUiId label;
  Vec3 from;
  Vec3 to;
  Vec3 label_offset;
  Vec2 label_size_pt;
  /* 1-based screen-order label: numbers left to right on top, letters top to
   * bottom on the right. Zero leaves the cell unlabeled. */
  uint32_t ordinal;
  bool8_t top_label;
} VkrEditorGridLine;

/* Where objects spawned in the Scene land (the Snapping menu). */
typedef enum VkrEditorSnapTarget {
  /* The ground plane under the pointer, else 8 m along its ray. */
  VKR_EDITOR_SNAP_FREE = 0,
  /* The first collision surface under the pointer, else the ground plane. */
  VKR_EDITOR_SNAP_SURFACE,
  /* The nearest grid point on the ground plane. */
  VKR_EDITOR_SNAP_GRID,
  VKR_EDITOR_SNAP_COUNT,
} VkrEditorSnapTarget;

typedef struct VkrEditorPlacement {
  VkrEditorSnapTarget target;
  /* Surface snaps turn the object's up axis to the surface normal. */
  bool8_t align_to_normal;
  /* Grid snaps take cell centers instead of line crossings. */
  bool8_t cell_centers;
  /* Distance from the surface or grid along its normal, world units. */
  float32_t offset;
  /* Turn about the snap normal, degrees. */
  float32_t yaw_degrees;
} VkrEditorPlacement;

/* A spawned object's world pose. */
#define VKR_EDITOR_COLOR_RECENT_MAX 8u

/* The color picker a Details swatch opened (editor_color.c). It edits a
 * display-encoded HSV color and writes the property's stored units back
 * through the swatch's VkrEditorDetails. */
typedef struct VkrEditorColorPicker {
  bool8_t open;
  struct VkrEditorDetails *details;
  VkrUiId field;
  const VkrPropertyDesc *property;
  Vec2 anchor_pt;
  /* Hue, saturation and value in [0, 1] of the display color; hue and
     saturation persist while value or saturation reach zero. */
  float32_t hue;
  float32_t saturation;
  float32_t value;
  float32_t alpha;
  /* A linear color past 1 keeps this scale; the picker edits the
     normalized color. */
  float32_t intensity;
  /* The stored value on opening; Escape restores it. */
  Vec4 original;
  /* 0 none, 1 the square, 2 the hue bar, 3 the alpha bar. */
  uint32_t drag;
  char hex[12];
  /* Display RGBA of colors picked this session, newest first. */
  Vec4 recent[VKR_EDITOR_COLOR_RECENT_MAX];
  uint32_t recent_count;
} VkrEditorColorPicker;

typedef struct VkrEditorDropPose {
  Vec3 position;
  VkrQuat rotation;
  /* Where the placing ray starts: the camera. */
  Vec3 eye;
} VkrEditorDropPose;

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
  /* New folder, New script, Import, Create and Refresh for the shown Content
     folder. */
  VKR_EDITOR_CONTEXT_CONTENT_FOLDER,
  /* The context entity's script slot (ADR-079): the loaded script types,
     New script, Edit script and Remove script. */
  VKR_EDITOR_CONTEXT_SCRIPT,
  /* Undo, Redo, Cut, Copy, Paste, Delete and Select All for the text field
     `context_text_field`. */
  VKR_EDITOR_CONTEXT_TEXT,
  /* The choices of the Details enum dropdown `context_details` asked for. */
  VKR_EDITOR_CONTEXT_CHOICE,
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
  VkrEditorBuild *build;
  VkrEditorContent *content;
  /* Project script modules and the Script editor window (ADR-079). */
  struct VkrEditorScripts *scripts;
  struct VkrEditorCode *code;
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
  /* A creation menu opened over the Scene places at this pixel; other
     creation menus place at the viewport's centre. */
  bool8_t context_at_pixel;
  Vec2 context_pixel;
  /* Component type a preset menu acts on. */
  const VkrTypeDesc *context_type;
  /* Details panel whose enum dropdown a choice menu acts on. */
  struct VkrEditorDetails *context_details;
  /* Text field a text menu acts on, and what it could offer when opened. */
  VkrUiId context_text_field;
  VkrUiTextFieldState context_text_state;
  VkrEditorViewportTab viewport_tabs[VKR_EDITOR_VIEWPORT_TAB_MAX];
  uint32_t viewport_tab_count;
  uint32_t viewport_tab_active;
  /* Rows the open menu showed last build and its laid-out height, which
     size its input region. */
  uint32_t context_count;
  float32_t context_height_pt;
  /* Keyboard-highlighted row of the menu, or -1. */
  int32_t context_cursor;
  /* One submenu, opened from row `context_sub_row`, beside the menu. */
  bool8_t context_sub_open;
  bool8_t context_sub_focused;
  VkrEditorContextKind context_sub_kind;
  uint32_t context_sub_row;
  int32_t context_sub_cursor;
  VkrUiRect context_sub_rect_px;
  /* Pointer position last build; hover follows the pointer only once it
     moves, so the keyboard highlight survives a resting pointer. */
  int32_t context_mouse_x;
  int32_t context_mouse_y;
  /* "New script" from an object's menu: once the module named
     `script_attach_module` loads, its first type becomes that object's
     script. An empty name waits for the Script editor to name it. */
  VkrEntityId script_attach_entity;
  char script_attach_module[48];
  /* A Script asset dropped on the Scene: the type it attaches while the
     pick under the drop runs. */
  const VkrTypeDesc *script_drop_type;
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
  /* Abstract one-per-world objects (sky, fog, post process) stacked at the
     origin; placed volumes, probes and markers; text; empty objects. */
  bool8_t labels_environment;
  bool8_t labels_markers;
  bool8_t labels_text;
  bool8_t labels_empty;
  VkrEditorPlacement placement;
  /* Empty objects of the primary scene, the World and each added scene, in
     the labels' container order. */
  VkrEditorLabelEmpties label_empties[2u + VKR_SCENE_ADDITIVE_MAX];
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
  /* A job-starting command holds the queue until its work settles. */
  bool8_t cmd_holding;
  /* The hold is a build, whose result the queue reports when it settles. */
  bool8_t cmd_holding_build;
  float64_t cmd_hold_seconds;
  /* Headless: quit once the queue has drained. */
  bool8_t cmd_quit_when_done;
  /* Borrowed buffer that collects [cmd] result lines while set. */
  char *cmd_capture;
  uint32_t cmd_capture_capacity;
  uint32_t cmd_capture_length;
  /* The agent channel and its operation table
     (docs/proposals/level-design-toolkit.md). */
  struct VkrEditorAgent *agent;
  /* The Level checks window's last report (editor_level.h). */
  struct VkrEditorLevelReport *level_report;
  /* Brush clipping: two clicks on the grid plane cut the selected brush
     with the vertical plane through them. */
  bool8_t clip_tool;
  bool8_t clip_has_first;
  Vec3 clip_first;
  Vec3 clip_current;
  /* The selected brush face's move handle, along its world normal from the
     face center; a drag moves the face by `face_drag_distance`. */
  bool8_t face_handle_valid;
  bool8_t face_dragging;
  Vec3 face_handle_center;
  Vec3 face_handle_normal;
  float32_t face_handle_length;
  float32_t face_drag_start;
  float32_t face_drag_distance;
  /* Terrain sculpting (editor_terrain.h): the tool, its settings, the
     stroke in progress and the ground under the pointer. */
  bool8_t terrain_tool;
  uint32_t terrain_mode;
  float32_t terrain_radius;
  float32_t terrain_strength;
  uint32_t terrain_layer;
  uint64_t terrain_stroke;
  uint64_t terrain_stroke_counter;
  float32_t terrain_flatten_height;
  bool8_t terrain_hit_valid;
  Vec3 terrain_hit;
  VkrEntityId terrain_hit_entity;
  float64_t terrain_last_time;
  /* Entity IO in Details (editor_io.h): a connection waiting for its target
     to be picked, and the sections' collapsed states. */
  VkrEntityId io_pick;
  bool8_t io_outputs_collapsed;
  bool8_t io_inputs_collapsed;
  bool8_t io_route_collapsed;
  /* Brush drawing: a left drag on the grid plane draws a box brush between
     the press and the release, one grid cell high. */
  bool8_t brush_draw;
  bool8_t brush_dragging;
  Vec3 brush_draw_start;
  Vec3 brush_draw_end;
  float32_t brush_draw_height;
  /* `ui.click`, `ui.drag` and `ui.key` input steps, one per frame:
   * {kind, x px, y px, button or key} with kind 0 move, 1 press, 2 release,
   * 3 key press, 4 key release. The queue holds until they run. */
  int32_t cmd_pointer_steps[32][4];
  uint32_t cmd_pointer_count;
  uint32_t cmd_pointer_next;
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
  /* Render scale while the screen-percentage slider is held, else zero. */
  float32_t view_scale_draft;
  VkrUiId grid_panel;
  uint64_t grid_frame;
  VkrSampleCameraView grid_camera_view;
  /* Right-label column width and top-label row height; each axis keeps out of
   * the other's strip so corner labels never collide. */
  Vec2 grid_reserved_pt;
  float32_t grid_spacing; /* Drawn world cell size; zero without a grid. */
  uint32_t grid_line_count;
  VkrEditorGridLine grid_lines[VKR_EDITOR_GRID_LINE_CAPACITY];
  VkrEditorColorPicker color_picker;
  /* Preferences window rows and scroll offset. */
  VkrEditorDetails preferences_details;
  float32_t preferences_scroll;
  float32_t preferences_height;
  VkrEditorAnimation animation;
  /* Additive scene path and sidecar a Cmd request borrows until the
     runtime consumes it (ADR-076). */
  char cmd_scene_path[1024];
  char cmd_scene_sidecar[1024];
  /* Intervals between the last frames, in milliseconds, for the Cmd `stats`
   * root; `frame_ms_next` wraps. */
  float32_t frame_ms[120];
  uint32_t frame_ms_count;
  uint32_t frame_ms_next;
  float64_t frame_last_time;

  VkrEditorWindowState windows[VKR_EDITOR_WINDOW_COUNT];
} VkrEditorUi;

void vkr_editor_ui_init(VkrEditorUi *editor);
VkrUiDockInputCapture vkr_editor_ui_build(VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame);
