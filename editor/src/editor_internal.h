#pragma once

#include "editor_ui.h"
#include "editor_workbench.h"

#define VKR_EDITOR_SCENE_TOOLBAR_LAYER 1u
#define VKR_EDITOR_VIEW_TOOLBAR_LAYER 2u
#define VKR_EDITOR_NAVIGATION_HEIGHT_PT VKR_UI_DOCK_TOOLBAR_PT
/* Context menus and the color picker sit above floating windows. */
#define VKR_EDITOR_POPUP_LAYER (VKR_EDITOR_WINDOW_COUNT + 3u)
/* Cmd bar suggestions sit above menus and floating windows. */
#define VKR_EDITOR_CMD_LAYER (VKR_EDITOR_WINDOW_COUNT + 4u)

/* Commands shared by the menu bar, toolbar and Cmd bar. */
typedef enum EditorCommand {
  CMD_LOAD,
  CMD_RELOAD,
  CMD_UNLOAD,
  CMD_SAVE,
  CMD_UNDO,
  CMD_REDO,
  CMD_FRAME,
  CMD_HIERARCHY,
  CMD_INSPECTOR,
  CMD_CONSOLE,
  CMD_BAKERY,
  CMD_CONTENT,
  CMD_ANIMATION,
  CMD_PHYSICS,
  CMD_RESET_LAYOUT,
  CMD_SIM_START,
  CMD_SIM_PAUSE,
  CMD_RENDER_START,
  CMD_RENDER_STOP,
  CMD_SIM_TOGGLE,
  CMD_SIM_STEP,
  CMD_SIM_RESET,
  CMD_RENDER_TOGGLE,
  CMD_CAMERA,
  CMD_GRAPHICS,
  CMD_DRAWS,
  CMD_MEMORY,
  CMD_LABELS,
  CMD_LABELS_DIRECTIONAL,
  CMD_LABELS_SPOT,
  CMD_LABELS_POINT,
  CMD_HELP,
  CMD_COMMANDS,
  CMD_ZOOM_IN,
  CMD_ZOOM_OUT,
  CMD_ZOOM_RESET,
  CMD_REDUCE_MOTION,
  /* Build menu (docs/proposals/project-packaging.md). */
  CMD_BUILD,
  CMD_BUILD_RUN,
  CMD_BUILD_SETTINGS,
  CMD_BUILD_OPEN,
  CMD_BUILD_LOG,
  CMD_SCENE_BAKE,
  /* The Script editor window (ADR-079). */
  CMD_SCRIPT_EDITOR,
  /* Agent changes awaiting review (ADR-084). */
  CMD_CHANGES,
  /* Brush drawing in the Scene (B). */
  CMD_BRUSH_DRAW,
  /* Brush clipping in the Scene. */
  CMD_BRUSH_CLIP,
  /* The Level checks window. */
  CMD_LEVEL_CHECKS,
  /* The Terrain window. */
  CMD_TERRAIN,
  /* The World Partition window. */
  CMD_PARTITION,
  /* The Environment window (ADR-098). */
  CMD_ENVIRONMENT,
  /* The Bake settings window (ADR-100). */
  CMD_BAKE_SETTINGS,
  /* The Lights window (ADR-100). */
  CMD_LIGHTS,
  /* Scene transform tools. */
  CMD_TOOL_SELECT,
  CMD_TOOL_MOVE,
  CMD_TOOL_ROTATE,
  CMD_TOOL_SCALE,
  /* Duplicate, delete, rename or snap the selection. */
  CMD_DUPLICATE,
  CMD_DELETE,
  CMD_RENAME,
  CMD_SNAP,
  /* Hide the selection, show only it, or show everything hidden, in the
     editor only (ADR-084). */
  CMD_HIDE,
  CMD_ISOLATE,
  CMD_REVEAL,
  /* Enter play, or leave it with a reset, as Unity's Play does. */
  CMD_PLAY,
  /* Workbench tabs by position, then the previous and next one. */
  CMD_WORKBENCH_1,
  CMD_WORKBENCH_2,
  CMD_WORKBENCH_3,
  CMD_WORKBENCH_4,
  CMD_WORKBENCH_5,
  CMD_WORKBENCH_6,
  CMD_WORKBENCH_7,
  CMD_WORKBENCH_8,
  CMD_WORKBENCH_9,
  CMD_WORKBENCH_PREV,
  CMD_WORKBENCH_NEXT,
  /* The Scene fills the window over the dock and the top bar, or returns. */
  CMD_SCENE_MAXIMIZE,
  CMD_COUNT
} EditorCommand;

VkrUiStyle vkr_editor_glass_style(void);
VkrUiStyle vkr_editor_overlay_style(void);
VkrUiWidgetConfig vkr_editor_text_config(float32_t size_pt, Vec4 color);
VkrUiWidgetConfig vkr_editor_menu_button_config(uint32_t column, bool8_t active,
                                                VkrFontHandle heading_font);

void vkr_editor_field_style(VkrUiWidgetConfig *config);
void vkr_editor_action_style(VkrUiWidgetConfig *config, VkrFontHandle heading);
void vkr_editor_primary_style(VkrUiWidgetConfig *config, VkrFontHandle heading);
void vkr_editor_ghost_style(VkrUiWidgetConfig *config);
void vkr_editor_toggle_style(VkrUiWidgetConfig *config, bool8_t active);
VkrUiWidgetConfig vkr_editor_icon_button_config(uint32_t column, uint32_t row,
                                                VkrUiIcon icon,
                                                String8 tooltip);
VkrUiWidgetConfig vkr_editor_section_label_config(VkrFontHandle heading);
/** Themed search field with a magnifier, placeholder and clear button.
 * Returns true when the text changed. */
bool8_t vkr_editor_search_field(VkrUiSystem *ui, String8 id,
                                VkrUiTextEditBuffer *buffer,
                                VkrUiPlacement placement, String8 placeholder,
                                String8 tooltip, VkrFontHandle font);

/* Points the top navigation bar covers at the top of the Scene: it lies
   over a Scene-only Scene and hides over a maximized one. */
float32_t vkr_editor_scene_top_pt(const VkrSampleUiFrame *frame);

void vkr_editor_dock_build(VkrEditorUi *editor, const VkrSampleUiFrame *frame);
void vkr_editor_labels_build(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame);
void vkr_editor_labels_project(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame);
/* Whether collision of a loaded scene (the open one, the World, added
   ones) lies between the camera of `view_projection` and `point`: a ray
   from the near plane under the point (depth 0, mat4_perspective and
   mat4_ortho_zo_yinv) to it. A point on a surface stays visible, and so
   does one behind the camera. `owner` and `entity` name an object whose
   own collision and its parent's hide nothing, so a mover or button does
   not hide its own icon; NULL and an invalid id name none. Geometry
   without collision, sensors and scenes with physics off hide nothing. */
bool8_t vkr_editor_label_occluded(const VkrSampleUiFrame *frame,
                                  Mat4 view_projection,
                                  Mat4 inverse_view_projection, Vec3 point,
                                  const VkrScene *owner, VkrEntityId entity);
/* An icon distance setting in metres, clamped to
   [0, VKR_EDITOR_LABEL_DISTANCE_MAX]; a value that is not finite gives the
   default. */
float32_t vkr_editor_label_distance(float32_t metres);
/* Whether any loaded scene has physics on, so collision can hide icons. */
bool8_t vkr_editor_label_occlusion_available(const VkrSampleUiFrame *frame);
/* Starts a build's icon visibility: `occlusion` and `max_distance` are the
   editor's settings, `delta_seconds` the frame time, `instant` skips fades
   (reduced motion). Last build's entries are copied to `scratch`, which
   must outlive vkr_editor_label_sights_end. */
void vkr_editor_label_sights_begin(VkrEditorLabelSights *sights,
                                   const VkrSampleUiFrame *frame,
                                   bool8_t occlusion, float32_t max_distance,
                                   float32_t delta_seconds, bool8_t instant,
                                   VkrAllocator *scratch);
/* The next icon in anchor order: `entity` of `scene`, at world `position`
   when `placed` (abstract icons stack at the origin and neither hide nor
   fade with distance). Returns the opacity to draw it with. */
float32_t vkr_editor_label_sight(VkrEditorLabelSights *sights,
                                 const VkrSampleUiFrame *frame,
                                 const VkrScene *scene, VkrEntityId entity,
                                 bool8_t placed, Vec3 position);
void vkr_editor_label_sights_end(VkrEditorLabelSights *sights);
void vkr_editor_dock_show(VkrUiDockTree *dock, VkrUiDockPanelKind kind);
/* Whether any tab of the tree, shown or not, holds `kind`. */
bool8_t vkr_editor_dock_has(const VkrUiDockTree *dock, VkrUiDockPanelKind kind);
/* The running Scene tool, and a switch to another that ends the one
   running and its pending clicks; the tools exclude each other. */
VkrEditorSceneTool vkr_editor_scene_tool(const VkrEditorUi *editor);
void vkr_editor_scene_tool_set(VkrEditorUi *editor, VkrEditorSceneTool tool);
/* Whether something in the Scene has a step for Escape: a running tool, a
   drag, an open Scene popup, a grid selection, an IO connection pick or the
   color picker. */
bool8_t vkr_editor_viewport_escape_armed(const VkrEditorUi *editor);
/* Multiple selection (ADR-089): the runtime's selected entity is the
   primary one; Ctrl+click (Cmd on macOS) adds or removes others. Update
   prunes dead entries once a build and clears them when the primary changes
   without a toggle. List writes the primary first. */
bool8_t vkr_editor_selection_modifier(const VkrSampleUiFrame *frame);
void vkr_editor_selection_update(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame);
void vkr_editor_selection_toggle(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 VkrEntityId entity);
void vkr_editor_selection_clear_extra(VkrEditorUi *editor);
/* Deletes (VKR_SCENE_EDIT_DELETE) or duplicates (VKR_SCENE_EDIT_DUPLICATE)
   every selected object of the primary's container as one undo step. A
   duplicate selects the copies once it applies. False with `message` when
   the selection cannot, or when another batch holds this build. */
bool8_t vkr_editor_selection_apply(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   VkrSceneEditAction action, char *message,
                                   uint64_t capacity);
bool8_t vkr_editor_selection_contains(const VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      VkrEntityId entity);
uint32_t vkr_editor_selection_list(const VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   VkrEntityId *out, uint32_t capacity);
/* Brush roles as operations name them: solid, visual, clip, trigger. */
extern const char *const vkr_editor_brush_roles[];
#define VKR_EDITOR_BRUSH_ROLE_COUNT 4u
/* The world box the brush tool is drawing. */
void vkr_editor_brush_draft_box(const VkrEditorUi *editor, Vec3 *out_lo,
                                Vec3 *out_hi);
/* `world` in window pixels through the Scene's view projection; false
   behind the eye. */
bool8_t vkr_editor_viewport_pixel(const VkrSampleUiFrame *frame, Vec3 world,
                                  Vec2 *out);
/* The size in points of `text` set in `font_handle` (the UI's default font
   when invalid) at `size` points. */
Vec2 vkr_editor_text_size(const VkrUiSystem *ui, VkrFontHandle font_handle,
                          const char *text, float32_t size);
/* The stairs the stairs tool would place, from `out_from` with the yaw
   `out_yaw` (degrees about +Y); false while no drag runs. */
bool8_t vkr_editor_path_stairs(const VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame,
                               SceneBlockout *out, Vec3 *out_from,
                               float32_t *out_yaw);
/* The `"role"`, `"surface"` and `"mark"` arguments of the next brush the
   editor creates, from the palette; the defaults go unsaid. */
void vkr_editor_brush_style(const VkrEditorUi *editor, char *out,
                            uint64_t size);
/* The open scene's name as the workbench row shows it. */
void vkr_editor_scene_label(const VkrEditorUi *editor,
                            const VkrSampleUiFrame *frame, char *out,
                            uint64_t size);

/* Workbench palettes (editor_workbench.c): rows of equal buttons under
   headings, two labelled columns when the panel is wide enough. */
typedef struct VkrEditorPalette {
  VkrEditorUi *editor;
  const VkrSampleUiFrame *frame;
  VkrUiSystem *ui;
  float32_t width;
  float32_t y;
  uint32_t columns;
  bool8_t labels;
  uint32_t slot;
} VkrEditorPalette;

/* A palette over `bounds` that starts `y` points down. */
VkrEditorPalette vkr_editor_palette_begin(VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame,
                                          VkrUiRect bounds, float32_t y);
void vkr_editor_palette_heading(VkrEditorPalette *palette, String8 id,
                                String8 text);
/* One button in the next slot; true when clicked while enabled. */
bool8_t vkr_editor_palette_button(VkrEditorPalette *palette, String8 id,
                                  const char *label, VkrUiIcon icon,
                                  String8 tooltip, bool8_t active,
                                  bool8_t disabled);
/* A button that runs `command`, with its shortcut in the tooltip. */
void vkr_editor_palette_command(VkrEditorPalette *palette, String8 id,
                                const char *label, VkrUiIcon icon,
                                EditorCommand command, bool8_t active);
/* A button that creates object kind `word` at the Scene's placement point,
   as the Create menu does. */
void vkr_editor_palette_create(VkrEditorPalette *palette, String8 id,
                               const char *label, VkrUiIcon icon,
                               const char *word);
/* Moves below the last row of buttons. */
void vkr_editor_palette_end(VkrEditorPalette *palette);

/* The dock panel that hosts floating window `kind`'s body, or
   VKR_UI_DOCK_PANEL_COUNT. While the live tree holds that panel, the window
   stays closed and opening it shows the tab. */
VkrUiDockPanelKind vkr_editor_window_dock_panel(VkrEditorWindowKind kind);
/* Whether window `kind`'s body shows, in its window or as a shown tab. */
bool8_t vkr_editor_window_shown(const VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame,
                                VkrEditorWindowKind kind);
void vkr_editor_dock_toggle(VkrUiDockTree *dock, VkrUiDockPanelKind kind);
/* Stats readout and render-failure card drawn over the Scene. */
void vkr_editor_scene_overlays_build(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame);
void vkr_editor_viewport_update(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame);
void vkr_editor_viewport_build(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame);
void vkr_editor_grid_build(VkrEditorUi *editor, const VkrSampleUiFrame *frame);
/* Asks the runtime to lift the ground grid onto the surface at the Scene's
   centre (VkrSampleGridFitRequest). */
void vkr_editor_view_fit_grid(const VkrSampleUiFrame *frame);
/* Where an object spawned at a viewport pixel lands under the Snapping
   settings; false without a valid view. `base` is the distance from the
   object's origin down to its lowest point, so it rests on the snap point
   instead of sinking into it. */
bool8_t vkr_editor_viewport_place(const VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame, Vec2 pixel,
                                  float32_t base, VkrEditorDropPose *out);
/* A decal's rotation on a surface of `normal` (ADR-101): its box's +Y
   along the normal and its image's top (local +Z) up a wall, or along
   `facing` on a floor or ceiling, then `angle` radians about the normal. */
VkrQuat vkr_editor_decal_orientation(Vec3 normal, Vec3 facing, float32_t angle);
/* The scatter the scatter paint tool paints: the selected scatter, or the
   parent of a selected scatter area; invalid otherwise (ADR-102). */
VkrEntityId vkr_editor_scatter_target(const VkrSampleUiFrame *frame);
/* Rests an existing object on what lies below it under the Snapping
   settings: the first collision surface, else the ground plane (Surface), the
   nearest grid point (Grid) or the ground plane (Free). Requests an undoable
   transform edit; false with `message` set when nothing lies below or the
   object cannot move. */
bool8_t vkr_editor_viewport_snap(const VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 VkrEntityId entity, char *message,
                                 uint64_t message_size);
/* Clickable world-axis indicator in the Scene's lower-left corner. */
void vkr_editor_orientation_gizmo_build(VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame);
void vkr_editor_grid_project(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame);
void vkr_editor_windows_register_input_layers(VkrEditorUi *editor,
                                              VkrUiSystem *ui);
void vkr_editor_windows_build_navigation(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame);
void vkr_editor_windows_build_floating(VkrEditorUi *editor, VkrUiSystem *ui,
                                       InputState *input,
                                       const VkrSampleUiFrame *frame);
void vkr_editor_windows_build_menu(VkrEditorUi *editor, VkrUiSystem *ui,
                                   const VkrSampleUiFrame *frame);

/* Containers (ADR-076): the scene and journal that own `entity`, the root
 * World for its id and the active scene otherwise; a frame view with them
 * substituted lets single-scene panels edit either container. */
const VkrScene *vkr_editor_entity_scene(const VkrSampleUiFrame *frame,
                                        VkrEntityId entity);
VkrSampleUiFrame vkr_editor_entity_frame(const VkrSampleUiFrame *frame,
                                         VkrEntityId entity);

/* Show a short notification; replaces any visible one. */
/* The world ray through a window pixel of the Scene: the camera's eye and a
   unit direction. */
bool8_t vkr_editor_viewport_ray(const VkrSampleUiFrame *frame, Vec2 pixel,
                                Vec3 *origin, Vec3 *direction);
void vkr_editor_toast(VkrEditorUi *editor, VkrUiIcon icon, Vec4 color,
                      const char *text);
void vkr_editor_toasts_build(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame);

/* Right-click menus (VkrEditorContextKind): open one of `kind` at a point;
   the caller then sets the target it acts on, such as `context_entity`. */
void vkr_editor_context_open(VkrEditorUi *editor, VkrEditorContextKind kind,
                             Vec2 position_pt);
/* Open the choice menu a Details enum dropdown asked for during the build
   that just ran, if any; call once after each Details panel. */
void vkr_editor_context_open_choice(VkrEditorUi *editor,
                                    struct VkrEditorDetails *details);
/* Open the color picker a Details swatch asked for during the build that
   just ran, if any; call once after each Details panel. */
void vkr_editor_color_picker_open(VkrEditorUi *editor,
                                  struct VkrEditorDetails *details);
/* The open color picker popup; call after the panels each frame. */
void vkr_editor_color_picker_build(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame);
VkrUiRect vkr_editor_context_menu_rect(const VkrEditorUi *editor,
                                       const VkrUiSystem *ui);
/* The open submenu's pixels, or an empty rect. */
VkrUiRect vkr_editor_context_submenu_rect(const VkrEditorUi *editor);
void vkr_editor_context_menu_build(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame);
/* Toggle an entity's own visibility through the undoable edit journal. */
void vkr_editor_toggle_visibility(const VkrSampleUiFrame *frame,
                                  VkrEntityId entity);
/* Hide and Isolate (ADR-084), never saved or undone. Whether the Scene draws
   `entity` by them: neither it nor an object above it is hidden, or with
   isolate one of them is isolated. */
bool8_t vkr_editor_hide_shown(const VkrEditorUi *editor, const VkrScene *scene,
                              VkrEntityId entity);
/* The Outliner eye: an object hidden in the editor shows again; one its
   Visibility hides shows through the undoable journal; a shown one hides
   in the editor only. */
void vkr_editor_hide_eye(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                         VkrEntityId entity);
/* Hides the selection, or with `isolate` shows only it. */
void vkr_editor_hide_selection(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame, bool8_t isolate);
/* Shows only `entity`, and what is under it. */
void vkr_editor_hide_isolate(VkrEditorUi *editor, VkrEntityId entity);
void vkr_editor_hide_reveal(VkrEditorUi *editor);
/* Sends a changed set to the runtime, without objects no longer alive. */
void vkr_editor_hide_publish(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame);

void vkr_editor_commands_update(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame);
bool8_t vkr_editor_command_enabled(EditorCommand command,
                                   const VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame);
/* A tooltip naming `command`'s shortcut after `text`, or after the command's
   name when `text` is NULL; frame-allocated. */
/* The first binding of `command` as its platform's shortcut text; false
   when it has none. */
bool8_t vkr_editor_command_shortcut(EditorCommand command, char *out,
                                    uint64_t size);
String8 vkr_editor_command_tooltip(VkrUiSystem *ui, EditorCommand command,
                                   const char *text);
void vkr_editor_command_execute(EditorCommand command, VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame);
void vkr_editor_window_set_visible(VkrEditorUi *editor,
                                   VkrEditorWindowKind kind, bool8_t visible);

/* Cmd bar: a typed command line with autocomplete, plus a script queue that
 * runs one command per frame (see docs/editor-cmd.md). */
/** Append `;`- or newline-separated commands; false when the queue is full. */
bool8_t vkr_editor_cmd_enqueue(VkrEditorUi *editor, const char *script);
/* Collects every [cmd] result line, newline-separated, into `buffer` until
   vkr_editor_cmd_capture_end, which returns the collected length. */
void vkr_editor_cmd_capture_begin(VkrEditorUi *editor, char *buffer,
                                  uint32_t capacity);
uint32_t vkr_editor_cmd_capture_end(VkrEditorUi *editor);
/* The queue ran every statement and no wait, hold or input step remains. */
bool8_t vkr_editor_cmd_idle(const VkrEditorUi *editor);
/** Applies the next `ui.click`, `ui.drag` or `ui.key` step to the host's
 * input. */
void vkr_editor_cmd_pointer_input(VkrEditorUi *editor, InputState *input);
/** Run due queued commands; call once per frame before panels build. */
void vkr_editor_cmd_update(VkrEditorUi *editor, const VkrSampleUiFrame *frame);
/** The top-bar field, in `column` of the navigation bar grid. */
void vkr_editor_cmd_bar_build(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame, uint32_t column);
/* Vocabulary words shared by commands and the evaluator, NULL-terminated and
 * index-aligned with their value tables. */
extern const char *const vkr_editor_cmd_camera_views[];
extern const char *const vkr_editor_cmd_render_modes[];
extern const VkrRenderMode vkr_editor_cmd_render_mode_values[];
extern const char *const vkr_editor_cmd_tools[];
extern const uint32_t vkr_editor_cmd_tool_modes[];
/* The Cmd `window` name of a floating window. */
const char *vkr_editor_cmd_window_name(VkrEditorWindowKind kind);
/** Evaluate one expression statement: `expr`, `name = expr` or
 * `path = expr`. Writes the result or error to `message`; false on error. */
bool8_t vkr_editor_cmd_eval(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                            String8 line, char *message, uint32_t capacity);
/** Completions for the expression that ends `text`: names at the start of a
 * path, members after a dot. Writes whole-line completions and hints. */
uint32_t vkr_editor_cmd_eval_complete(VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      String8 text, char (*lines)[96],
                                      char (*hints)[96], uint32_t capacity);
/** Autocomplete suggestions beneath the field; build after other overlays. */
void vkr_editor_cmd_suggestions_build(VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame);
