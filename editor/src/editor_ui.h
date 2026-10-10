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
  /* Agent changes awaiting review (ADR-084). */
  VKR_EDITOR_WINDOW_CHANGES,
  /* Level checks against the player capsule. */
  VKR_EDITOR_WINDOW_LEVEL,
  /* Terrain sculpting and painting. */
  VKR_EDITOR_WINDOW_TERRAIN,
  /* World partition cells (ADR-086). */
  VKR_EDITOR_WINDOW_PARTITION,
  /* The scene's environment and its presets (ADR-098). */
  VKR_EDITOR_WINDOW_ENVIRONMENT,
  /* Lightmap and diffuse volume bake settings (ADR-100). */
  VKR_EDITOR_WINDOW_BAKE_SETTINGS,
  /* The scene's lights and light groups (ADR-100). */
  VKR_EDITOR_WINDOW_LIGHTS,
  VKR_EDITOR_WINDOW_COUNT,
} VkrEditorWindowKind;

typedef struct VkrEditorWindowState {
  Vec2 position_pt;
  Vec2 size_pt;
  uint32_t z_order;
  bool8_t visible;
  bool8_t dragging;
  Vec2 drag_grab_pt;
  /* The right and bottom borders resize every window; the held ones
     (VKR_EDITOR_WINDOW_EDGE_*), or zero. */
  uint8_t resize_edges;
} VkrEditorWindowState;

#define VKR_EDITOR_WINDOW_EDGE_RIGHT 1u
#define VKR_EDITOR_WINDOW_EDGE_BOTTOM 2u

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
  /* Faded opacity the build drew the icon with; projection keeps an icon
     under VKR_EDITOR_LABEL_ALPHA_MIN off the Scene. */
  float32_t alpha;
} VkrEditorLabelAnchor;

/* Icons fainter than this are not drawn and take no clicks. */
#define VKR_EDITOR_LABEL_ALPHA_MIN 0.02f
/* Metres from the camera past which icons fade out: the default and the
   largest a setting takes; 0 is no limit. */
#define VKR_EDITOR_LABEL_DISTANCE_DEFAULT 80.0f
#define VKR_EDITOR_LABEL_DISTANCE_MAX 500.0f
/* Icons the UI can build at most: one node each. */
#define VKR_EDITOR_LABEL_SIGHT_MAX VKR_UI_FRAME_NODE_CAPACITY
/* Occlusion rays one build casts at most. */
#define VKR_EDITOR_LABEL_RAYS_PER_FRAME 48u

/* One icon as the last build left it, in label anchor order: its faded
   opacity and whether collision hid it from the camera when a ray last
   tested it. */
typedef struct VkrEditorLabelSight {
  VkrEntityId entity;
  float32_t alpha;
  bool8_t occluded;
  /* A ray has tested the icon since it appeared. */
  bool8_t tested;
} VkrEditorLabelSight;

/* What decides whether earlier ray results still hold: the camera, the
   loaded containers and their structure and edit revisions, and whether
   occlusion applies. Zeroed before it is filled, so it compares as bytes. */
typedef struct VkrEditorLabelSightKey {
  Mat4 view_projection;
  const struct VkrScene *scenes[2u + VKR_SCENE_ADDITIVE_MAX];
  uint64_t revisions[2u + VKR_SCENE_ADDITIVE_MAX];
  uint64_t edits[2u + VKR_SCENE_ADDITIVE_MAX];
  uint64_t generation;
  bool8_t occlusion;
} VkrEditorLabelSightKey;

/* Icon visibility kept across builds (editor_label_sight.c). A build
   matches each icon to its entry from the last build, tests a bounded
   number of icons per frame round robin while the key changes, and fades
   each icon's opacity toward its target. */
typedef struct VkrEditorLabelSights {
  VkrEditorLabelSight entries[VKR_EDITOR_LABEL_SIGHT_MAX];
  uint32_t count;
  /* The last build's entries, copied to frame scratch, while this build
     matches them; NULL outside a build. */
  const VkrEditorLabelSight *previous;
  uint32_t previous_count;
  uint32_t match;
  /* Next icon the sweep tests and how many icons it still visits; zero
     when every icon has been tested under the current key. */
  uint32_t cursor;
  uint32_t sweep;
  uint32_t rays_left;
  /* Seconds since a sweep ended; a quiet key still gets a sweep each
     second, for collision that builds after a load or moves without an
     edit. */
  float32_t idle_seconds;
  VkrEditorLabelSightKey key;
  /* This build's inputs. */
  Mat4 inverse_view_projection;
  float32_t delta_seconds;
  float32_t max_distance;
  bool8_t occlusion;
  bool8_t perspective;
  bool8_t instant;
  /* Agent-visible counts, read between builds: icons the last build made
     and those collision hides, and icons the last projection placed on the
     Scene. */
  uint32_t icon_count;
  uint32_t occluded_count;
  uint32_t shown_count;
} VkrEditorLabelSights;

typedef struct VkrEditorPhysicsLine VkrEditorPhysicsLine;

/* A size label of the Scene overlay (editor_physics.c): the dimensions of a
   selected or drawn brush, or the measure tool's reading. The build sizes
   it; the projection centers it just above `anchor`. */
typedef struct VkrEditorMeasureLabel {
  VkrUiId widget;
  Vec3 anchor;
  Vec2 size_pt;
} VkrEditorMeasureLabel;

/* The selection's labels, the drawn box's and the measurement's. */
#define VKR_EDITOR_MEASURE_LABEL_MAX 20u

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

/* Face handles one selected brush can show; VKR_BRUSH_FACE_MAX. */
#define VKR_EDITOR_FACE_HANDLE_MAX 64u

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
  /* Moved and drawn brushes snap to nearby brushes (editor_level.h). */
  bool8_t magnet;
  /* Move handles and arrow nudges keep the moved object's box corner on the
     grid, or its origin when it has no box. */
  bool8_t move_grid;
  /* Rotate handles turn in VKR_EDITOR_TURN_STEP_DEGREES steps. */
  bool8_t turn_steps;
} VkrEditorPlacement;

#define VKR_EDITOR_TURN_STEP_DEGREES 15.0f

/* Objects one selection holds: the primary and the extra ones. */
#define VKR_EDITOR_SELECTION_MAX 16u
/* Dabs one scatter paint stroke holds. */
#define VKR_EDITOR_SCATTER_DAB_MAX 128u

/* Workbenches (ADR-089): the tabs under the top bar. Each holds a dock
   layout, the floating windows open in it and the Scene's editing mode for
   one task. A kind is a built-in workbench: its palette, icon and default
   layout; custom workbenches copy one. */
typedef enum VkrEditorWorkbenchKind {
  VKR_EDITOR_WORKBENCH_GENERAL = 0,
  VKR_EDITOR_WORKBENCH_LEVEL_DESIGN,
  VKR_EDITOR_WORKBENCH_TERRAIN,
  VKR_EDITOR_WORKBENCH_LIGHTING,
  VKR_EDITOR_WORKBENCH_SCRIPTING,
  /* Materials and the art pass (docs/proposals/artist-toolkit.md). */
  VKR_EDITOR_WORKBENCH_ART,
  VKR_EDITOR_WORKBENCH_COUNT,
} VkrEditorWorkbenchKind;

/* The Scene tool running in a workbench; the tools exclude each other. */
typedef enum VkrEditorSceneTool {
  VKR_EDITOR_SCENE_TOOL_NONE = 0,
  VKR_EDITOR_SCENE_TOOL_BRUSH_DRAW,
  VKR_EDITOR_SCENE_TOOL_CLIP,
  VKR_EDITOR_SCENE_TOOL_TERRAIN,
  /* Two clicks on the grid plane: where stairs or a corridor start and
     end. */
  VKR_EDITOR_SCENE_TOOL_STAIRS,
  VKR_EDITOR_SCENE_TOOL_CORRIDOR,
  /* Two clicks on surfaces: the distance between them. */
  VKR_EDITOR_SCENE_TOOL_MEASURE,
  /* Clicks on surfaces place decals facing them (ADR-101). */
  VKR_EDITOR_SCENE_TOOL_DECAL,
  /* Strokes paint the selected scatter's areas (ADR-102). */
  VKR_EDITOR_SCENE_TOOL_SCATTER,
  VKR_EDITOR_SCENE_TOOL_COUNT,
} VkrEditorSceneTool;

typedef struct VkrEditorWorkbenchMode {
  /* VkrGizmoMode of the transform tool. */
  uint32_t gizmo_tool;
  VkrEditorSceneTool scene_tool;
  VkrEditorSnapTarget snap;
  bool8_t grid;
} VkrEditorWorkbenchMode;

/* Tabs a row holds: the built-ins and custom copies, one per Ctrl+digit. */
#define VKR_EDITOR_WORKBENCH_MAX 9u
#define VKR_EDITOR_WORKBENCH_ID_CAPACITY 24u
#define VKR_EDITOR_WORKBENCH_NAME_CAPACITY 32u

typedef struct VkrEditorWorkbench {
  /* Stable id: a built-in's word, or custom_<n> for a copy. */
  char id[VKR_EDITOR_WORKBENCH_ID_CAPACITY];
  char name[VKR_EDITOR_WORKBENCH_NAME_CAPACITY];
  VkrEditorWorkbenchKind kind;
  bool8_t custom;
  /* The project scene the tab shows, once it has shown one: an id, or empty
     for the World alone. Switching to the tab opens it. */
  char scene_id[37];
  bool8_t scene_known;
  /* The dock tree while another workbench is active; the active one's live
     tree is the frame's dock. */
  VkrUiDockTree layout;
  /* One bit per VkrEditorWindowKind open in the workbench. */
  uint32_t windows;
  VkrEditorWorkbenchMode mode;
} VkrEditorWorkbench;

typedef struct VkrEditorWorkbenches {
  /* In tab order. */
  VkrEditorWorkbench items[VKR_EDITOR_WORKBENCH_MAX];
  uint32_t count;
  uint32_t active;
  /* A switch that applies at the start of the next UI build, or
     UINT32_MAX. */
  uint32_t requested;
  /* The tab whose name its inline field edits, or UINT32_MAX, the text and
     whether the field still has to take focus. */
  uint32_t renaming;
  char rename_text[VKR_EDITOR_WORKBENCH_NAME_CAPACITY];
  bool8_t rename_focus;
  /* The number of the next custom id. */
  uint32_t next_custom;
  /* The last tab clicked and when, so a second click renames it. */
  uint32_t click_tab;
  float64_t click_time;
} VkrEditorWorkbenches;

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
  /* The workbench tab `context_panel` names: switch to it or reset it. */
  VKR_EDITOR_CONTEXT_WORKBENCH,
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
  /* Material graphs and files, and their journal (editor_material.h). */
  struct VkrEditorMaterials *materials;
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
  /* Icons behind collision fade out; icons fade out over the last fifth of
     `labels_max_distance` metres from the camera, 0 for no limit. */
  bool8_t labels_occlusion;
  float32_t labels_max_distance;
  VkrEditorPlacement placement;
  VkrEditorWorkbenches workbenches;
  /* Empty objects of the primary scene, the World and each added scene, in
     the labels' container order. */
  VkrEditorLabelEmpties label_empties[2u + VKR_SCENE_ADDITIVE_MAX];
  /* Frame-scratch records, consumed before UI geometry preparation. */
  VkrEditorLabelAnchor *label_anchors;
  uint32_t label_anchor_count;
  VkrEditorLabelSights label_sights;
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
  /* Holds an --exec script as long as a Windows command line (32767
     characters), such as a measured flight of a few hundred camera moves. */
  char cmd_queue[32768];
  uint32_t cmd_queue_length;
  uint32_t cmd_queue_offset;
  float64_t cmd_wait_seconds;
  float64_t cmd_wait_scene_seconds;
  /* A job-starting command holds the queue until its work settles. */
  bool8_t cmd_holding;
  /* The hold is a build, whose result the queue reports when it settles. */
  bool8_t cmd_holding_build;
  /* The hold is a lighting bake (`scene.bake`), whose outcome the queue
     reports when it settles. */
  bool8_t cmd_holding_bake;
  /* An `op` statement holds the queue until its operation answers. */
  bool8_t cmd_holding_op;
  float64_t cmd_hold_seconds;
  /* Headless: quit once the queue has drained. */
  bool8_t cmd_quit_when_done;
  /* No window: no one watches its frames, so long agent checks may
     take more of a build (ADR-084). */
  bool8_t headless;
  /* Borrowed buffer that collects [cmd] result lines while set. */
  char *cmd_capture;
  uint32_t cmd_capture_capacity;
  uint32_t cmd_capture_length;
  /* The agent channel and its operation table
     (ADR-084). */
  struct VkrEditorAgent *agent;
  /* The collaborative editing session, idle until hosted or joined
     (editor_session.h). */
  struct VkrEditorSession *session;
  /* The Level checks window's last report (editor_level.h). */
  struct VkrEditorLevelReport *level_report;
  /* Brush clipping on the selected brush's grid (editor_brush_grid.h). */
  bool8_t clip_tool;
  /* Brush face handles (ADR-084): one per face of a brush selected with
     the Select tool, or of the selected face alone, along the face's world
     normal from its center. `face_handle_hot` is the handle under the
     pointer or being dragged, -1 for none; a drag moves its face by
     `face_drag_distance`. */
  uint32_t face_handle_count;
  VkrEntityId face_handle_faces[VKR_EDITOR_FACE_HANDLE_MAX];
  Vec3 face_handle_centers[VKR_EDITOR_FACE_HANDLE_MAX];
  Vec3 face_handle_normals[VKR_EDITOR_FACE_HANDLE_MAX];
  float32_t face_handle_length;
  int32_t face_handle_hot;
  bool8_t face_dragging;
  VkrEntityId face_drag_face;
  float32_t face_drag_start;
  float32_t face_drag_distance;
  /* Brush grid editing of the selected brush (editor_brush_grid.h). */
  struct VkrEditorBrushGrid *brush_grid;
  /* The brushes a moved or drawn brush snaps to (editor_level.h). */
  struct VkrEditorMagnet *magnet;
  /* Handles of the selected blockout shape (editor_blockout.h). */
  struct VkrEditorBlockout *blockout;
  /* Drag handles on light and volume outlines (editor_lighting.c). */
  struct VkrEditorLightingHandles *lighting_handles;
  /* Terrain sculpting (editor_terrain.h): the tool, its settings, the
     stroke in progress and the ground under the pointer. */
  bool8_t terrain_tool;
  /* The stairs or corridor tool, else NONE, and its start once clicked. */
  VkrEditorSceneTool path_tool;
  /* The points clicked so far (stairs: where a drag started; corridors: up
     to 16 floor points) and the pointer's point on their plane. */
  uint32_t path_count;
  Vec3 path_points[16];
  Vec3 path_current;
  /* The measure tool: up to two clicked points, and the surface point
     under the pointer, which ends the line while one point is down. A
     third click starts over; leaving the tool clears it. */
  bool8_t measure_tool;
  uint32_t measure_count;
  Vec3 measure_points[2];
  Vec3 measure_hover;
  bool8_t measure_hover_valid;
  /* The decal tool (ADR-101): the box a click places, in metres (width
     and height across the surface, depth along its normal) and degrees
     about the normal, and the surface point under the pointer with its
     normal and the view's forward, which orients a decal on a floor. */
  bool8_t decal_tool;
  float32_t decal_size;
  float32_t decal_depth;
  float32_t decal_angle;
  bool8_t decal_hover_valid;
  Vec3 decal_hover;
  Vec3 decal_hover_normal;
  Vec3 decal_hover_facing;
  /* The scatter paint tool (ADR-102): the brush's radius in metres, the
     density in copies per square metre and the spacing between copies its
     areas ask for; the stroke's dabs on surfaces, sent as one
     scatter.paint when the button is released, erasing when Shift held
     at the press; and the surface point under the pointer. */
  bool8_t scatter_tool;
  float32_t scatter_radius;
  float32_t scatter_density;
  float32_t scatter_spacing;
  bool8_t scatter_stroking;
  bool8_t scatter_erasing;
  uint32_t scatter_dab_count;
  Vec3 scatter_dabs[VKR_EDITOR_SCATTER_DAB_MAX];
  bool8_t scatter_hover_valid;
  Vec3 scatter_hover;
  /* The scale figure, for this session: the player's capsule standing on
     the surface under the pointer while it rests over the Scene. */
  bool8_t scale_figure;
  bool8_t scale_figure_valid;
  Vec3 scale_figure_at;
  /* Frame-scratch size labels of the Scene overlay. */
  VkrEditorMeasureLabel measure_labels[VKR_EDITOR_MEASURE_LABEL_MAX];
  uint32_t measure_label_count;
  /* The palette's stairs kind (SceneStairsKind), which way turning
     stairs turn, and whether corridors curve through their points. */
  uint32_t stairs_kind;
  bool8_t stairs_left;
  bool8_t corridor_curved;
  /* Objects selected beside the runtime's selection, the primary one, by
     Ctrl+click (Cmd on macOS) in the Outliner or the Scene. A change of the
     primary selection that no toggle made clears them. */
  VkrEntityId selection_extra[VKR_EDITOR_SELECTION_MAX];
  uint32_t selection_extra_count;
  VkrEntityId selection_primary;
  bool8_t selection_keep;
  /* The selection-wide delete or duplicate in flight, or zero; its copies
     become the selection when it applies. */
  uint64_t selection_batch_token;
  bool8_t selection_batch_duplicates;
  /* The role (index into vkr_editor_brush_roles) of the next brush the
     Create path, box drawing or the stairs tool makes. */
  uint32_t brush_role;
  /* Its surface tag and mark (vkr_surface.h): the palette's last picks. */
  VkrSurface brush_surface;
  VkrSurfaceMark brush_mark;
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
  /* Brush drawing: a left drag outlines the base of a box brush on the
     plane the Snapping menu picks, the pointer then sets its height and a
     click creates it. */
  bool8_t brush_draw;
  bool8_t brush_dragging;
  /* After the base drag, the pointer raises the box until a click creates
     it; `brush_draw_raise_start` is where along the vertical the raise
     began. */
  bool8_t brush_raising;
  float32_t brush_draw_raise_start;
  Vec3 brush_draw_start;
  Vec3 brush_draw_end;
  float32_t brush_draw_height;
  /* An orthographic view draws the box flat: `brush_draw_start` and
     `brush_draw_end` are opposite corners, the drag sets the two screen
     axes and the release creates it (vkr_editor_brush_draft_box). */
  bool8_t brush_draw_flat;
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
  /* The pointer is over the Scene image and no window, menu, popup or
   * toolbar covers it; Scene tools take presses only then. */
  bool8_t scene_pointer_free;
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
  /* Hide and Isolate (ADR-084), for this session only: objects the Scene
     neither draws nor picks, or with `hidden_isolate` the only ones it
     draws; sent to the runtime while `hidden_dirty`. */
  VkrEntityId hidden[VKR_SCENE_EDITOR_HIDDEN_MAX];
  uint32_t hidden_count;
  bool8_t hidden_isolate;
  bool8_t hidden_dirty;
  /* The view's grid step at the last build: drags the runtime applies after
     the build snap moves to it. */
  float32_t move_step;
  uint32_t grid_line_count;
  VkrEditorGridLine grid_lines[VKR_EDITOR_GRID_LINE_CAPACITY];
  VkrEditorColorPicker color_picker;
  /* Preferences window rows and scroll offset. */
  VkrEditorDetails preferences_details;
  float32_t preferences_scroll;
  float32_t preferences_height;
  /* Environment window (ADR-098): rows, scroll, collapsed parts, the
     preset documents listed under assets/environments, the edits an Apply
     lends to the runtime until it dispatches them, and the last outcome. */
  VkrEditorDetails environment_details;
  float32_t environment_scroll;
  float32_t environment_height;
  bool8_t environment_collapsed[9];
  char environment_presets[32][256];
  uint32_t environment_preset_count;
  float64_t environment_listed_at;
  VkrSampleEditBatchItem environment_items[9];
  uint64_t environment_batch_token;
  char environment_message[192];
  /* Bake settings window rows and collapsed groups (ADR-100). */
  VkrEditorDetails bake_settings_details;
  bool8_t bake_settings_collapsed[2];
  /* Lights window: the selected light's rows, scroll and content height. */
  VkrEditorDetails lights_details;
  float32_t lights_scroll;
  float32_t lights_height;
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
