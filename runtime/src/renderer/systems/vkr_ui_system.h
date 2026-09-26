/**
 * @file vkr_ui_system.h
 * @brief Immediate-mode UI backed by retained widget and text state.
 */
#pragma once

#include "core/input.h"
#include "core/ui/vkr_ui_draw.h"
#include "core/ui/vkr_ui_grid.h"
#include "core/ui/vkr_ui_id.h"
#include "core/ui/vkr_ui_style.h"
#include "core/ui/vkr_ui_tile.h"
#include "core/vkr_window.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_dmemory.h"
#include "renderer/resources/ui/vkr_ui_text.h"

typedef struct VkrFontSystem VkrFontSystem;
typedef struct VkrWindow VkrWindow;
typedef struct VkrUiRetainedState VkrUiRetainedState;
typedef struct VkrUiFrameNode VkrUiFrameNode;
typedef struct VkrPreparedUiDrawList VkrPreparedUiDrawList;

#define VKR_UI_FRAME_NODE_CAPACITY 2048u
#define VKR_UI_RETAINED_BUCKET_CAPACITY 4096u
#define VKR_UI_RETAINED_GRACE_FRAMES 120u
#define VKR_UI_CONTAINER_STACK_CAPACITY 32u

typedef struct VkrUiInputCapture {
  bool8_t mouse;
  bool8_t keyboard;
  bool8_t text;
  VkrUiId hot_id;
  VkrUiId active_id;
} VkrUiInputCapture;

/** Grid placement authored in logical points. */
typedef struct VkrUiPlacement {
  uint32_t column;
  uint32_t row;
  uint32_t column_span;
  uint32_t row_span;
  VkrUiAlign justify;
  VkrUiAlign align;
  VkrUiEdges margin_pt;
} VkrUiPlacement;

#define VKR_UI_PLACEMENT_DEFAULT                                               \
  (VkrUiPlacement) {                                                           \
    .column = VKR_UI_GRID_AUTO, .row = VKR_UI_GRID_AUTO, .column_span = 1u,    \
    .row_span = 1u, .justify = VKR_UI_ALIGN_STRETCH,                           \
    .align = VKR_UI_ALIGN_STRETCH                                              \
  }

/** Track declarations and box style for a panel or scroll container. */
typedef struct VkrUiPanelConfig {
  VkrUiPlacement placement;
  const VkrUiTrack *columns;
  uint32_t column_count;
  const VkrUiTrack *rows;
  uint32_t row_count;
  VkrUiStyle style;
  bool8_t clip_children;
} VkrUiPanelConfig;

/** Phosphor icon glyphs (MIT) drawn from the cooked MTSDF icon atlases set by
 * vkr_ui_system_set_fonts. Without icon fonts, icons draw nothing. */
typedef enum VkrUiIcon {
  VKR_UI_ICON_NONE = 0,
  VKR_UI_ICON_PLAY,
  VKR_UI_ICON_PAUSE,
  VKR_UI_ICON_MONITOR_PLAY,
  VKR_UI_ICON_MONITOR_STOP,
  VKR_UI_ICON_HIERARCHY,
  VKR_UI_ICON_INSPECTOR,
  VKR_UI_ICON_CONSOLE,
  VKR_UI_ICON_SCENE,
  VKR_UI_ICON_BAKERY,
  VKR_UI_ICON_SCENE_LOAD,
  VKR_UI_ICON_SCENE_UNLOAD,
  VKR_UI_ICON_CAMERA,
  VKR_UI_ICON_GRIP,
  VKR_UI_ICON_LOG_FATAL,
  VKR_UI_ICON_LOG_ERROR,
  VKR_UI_ICON_LOG_WARNING,
  VKR_UI_ICON_LOG_INFO,
  VKR_UI_ICON_LOG_DEBUG,
  VKR_UI_ICON_LOG_TRACE,
  VKR_UI_ICON_PROJECT,
  VKR_UI_ICON_FOLDER,
  VKR_UI_ICON_CONTENT,
  VKR_UI_ICON_TEXTURE,
  VKR_UI_ICON_MATERIAL,
  VKR_UI_ICON_MESH,
  VKR_UI_ICON_FONT,
  VKR_UI_ICON_LIGHT,
  VKR_UI_ICON_ENVIRONMENT,
  VKR_UI_ICON_PROBE,
  VKR_UI_ICON_REFRESH,
  VKR_UI_ICON_ADD,
  VKR_UI_ICON_SEARCH,
  VKR_UI_ICON_STOP,
  VKR_UI_ICON_STEP,
  VKR_UI_ICON_CHEVRON_DOWN,
  VKR_UI_ICON_CHEVRON_RIGHT,
  VKR_UI_ICON_CHEVRON_LEFT,
  VKR_UI_ICON_CHEVRON_UP,
  VKR_UI_ICON_DISCLOSURE_OPEN,
  VKR_UI_ICON_DISCLOSURE_CLOSED,
  VKR_UI_ICON_CLOSE,
  VKR_UI_ICON_CHECK,
  VKR_UI_ICON_MINUS,
  VKR_UI_ICON_MORE,
  VKR_UI_ICON_MENU,
  VKR_UI_ICON_ARROW_LEFT,
  VKR_UI_ICON_ARROW_RIGHT,
  VKR_UI_ICON_ARROW_UP,
  VKR_UI_ICON_SORT_ASCENDING,
  VKR_UI_ICON_SORT_DESCENDING,
  VKR_UI_ICON_EYE,
  VKR_UI_ICON_EYE_SLASH,
  VKR_UI_ICON_LOCK,
  VKR_UI_ICON_UNLOCK,
  VKR_UI_ICON_TRASH,
  VKR_UI_ICON_COPY,
  VKR_UI_ICON_PASTE,
  VKR_UI_ICON_DUPLICATE,
  VKR_UI_ICON_RENAME,
  VKR_UI_ICON_SAVE,
  VKR_UI_ICON_UNDO,
  VKR_UI_ICON_REDO,
  VKR_UI_ICON_RESET,
  VKR_UI_ICON_FRAME,
  VKR_UI_ICON_SETTINGS,
  VKR_UI_ICON_GRAPHICS,
  VKR_UI_ICON_METRICS,
  VKR_UI_ICON_MEMORY,
  VKR_UI_ICON_DRAWS,
  VKR_UI_ICON_HELP,
  VKR_UI_ICON_COMMAND,
  VKR_UI_ICON_KEYBOARD,
  VKR_UI_ICON_ANIMATION,
  VKR_UI_ICON_PHYSICS,
  VKR_UI_ICON_COLLIDER,
  VKR_UI_ICON_RIGID_BODY,
  VKR_UI_ICON_JOINT,
  VKR_UI_ICON_BONE,
  VKR_UI_ICON_SKIN,
  VKR_UI_ICON_CAMERA_ENTITY,
  VKR_UI_ICON_DIRECTIONAL_LIGHT,
  VKR_UI_ICON_SPOT_LIGHT,
  VKR_UI_ICON_POINT_LIGHT,
  VKR_UI_ICON_RECT_LIGHT,
  VKR_UI_ICON_SKY,
  VKR_UI_ICON_FOG,
  VKR_UI_ICON_VOLUME,
  VKR_UI_ICON_EMPTY,
  VKR_UI_ICON_MOVE,
  VKR_UI_ICON_ROTATE,
  VKR_UI_ICON_SCALE,
  VKR_UI_ICON_SELECT,
  VKR_UI_ICON_SNAP,
  VKR_UI_ICON_WORLD,
  VKR_UI_ICON_LOCAL,
  VKR_UI_ICON_GRID,
  VKR_UI_ICON_PERSPECTIVE,
  VKR_UI_ICON_VIEW_MODE,
  VKR_UI_ICON_LAYERS,
  VKR_UI_ICON_IMPORT,
  VKR_UI_ICON_EXPORT,
  VKR_UI_ICON_REVEAL,
  VKR_UI_ICON_SPINNER,
  VKR_UI_ICON_BELL,
  VKR_UI_ICON_CPU,
  VKR_UI_ICON_GPU,
  VKR_UI_ICON_SPEED,
  VKR_UI_ICON_FILTER,
  VKR_UI_ICON_LIST,
  VKR_UI_ICON_FILE,
  VKR_UI_ICON_PIN,
  VKR_UI_ICON_MAXIMIZE,
  VKR_UI_ICON_MINIMIZE,
  VKR_UI_ICON_LAYOUT,
  VKR_UI_ICON_CHECK_CIRCLE,
  VKR_UI_ICON_PLANET,
  VKR_UI_ICON_RECORD,
  VKR_UI_ICON_PLUS_CIRCLE,
  VKR_UI_ICON_SIDEBAR,
  VKR_UI_ICON_WINDOW,
  VKR_UI_ICON_TAG,
  VKR_UI_ICON_HOME,
  VKR_UI_ICON_DOT,
  VKR_UI_ICON_ARROW_DOWN,
  VKR_UI_ICON_FOLLOW_TAIL,
  VKR_UI_ICON_BROOM,
  VKR_UI_ICON_CROSSHAIR,
  VKR_UI_ICON_HAND,
  VKR_UI_ICON_WRENCH,
  VKR_UI_ICON_PALETTE,
  VKR_UI_ICON_THERMOMETER,
  VKR_UI_ICON_ANGLE,
  VKR_UI_ICON_RULER,
  VKR_UI_ICON_TIMER,
  VKR_UI_ICON_GRAPH,
  VKR_UI_ICON_SPARKLE,
  VKR_UI_ICON_ZOOM_IN,
  VKR_UI_ICON_ZOOM_OUT,
  VKR_UI_ICON_CARET_UP_DOWN,
  VKR_UI_ICON_KEBAB,
  VKR_UI_ICON_CHART_BAR,
  VKR_UI_ICON_LIGHTNING,
  VKR_UI_ICON_GAME_CONTROLLER,
  VKR_UI_ICON_PERSON_WALK,
  VKR_UI_ICON_SHAPES,
  VKR_UI_ICON_DATABASE,
  VKR_UI_ICON_HARD_DRIVES,
  VKR_UI_ICON_CLOCK,
  VKR_UI_ICON_BRAND,
  VKR_UI_ICON_EYE_FILL,
  VKR_UI_ICON_LOCK_FILL,
  VKR_UI_ICON_WARNING_FILL,
  VKR_UI_ICON_INFO_FILL,
  VKR_UI_ICON_CHECK_SQUARE,
  VKR_UI_ICON_SQUARE,
  VKR_UI_ICON_CIRCLE,
  VKR_UI_ICON_SUN_DIM,
  VKR_UI_ICON_MOON,
  VKR_UI_ICON_CLOUD,
  VKR_UI_ICON_DROP,
  VKR_UI_ICON_WAVES,
  VKR_UI_ICON_TREE,
  VKR_UI_ICON_BUILDINGS,
  VKR_UI_ICON_SELECTION,
  VKR_UI_ICON_TEXT,
  VKR_UI_ICON_CODE,
  VKR_UI_ICON_TERMINAL,
  VKR_UI_ICON_GIT_BRANCH,
  VKR_UI_ICON_PUZZLE,
  VKR_UI_ICON_STAR,
  VKR_UI_ICON_STAR_FILL,
  VKR_UI_ICON_PENCIL_LINE,
  VKR_UI_ICON_BOUNDING_BOX,
  VKR_UI_ICON_VIDEO,
  VKR_UI_ICON_SPEAKER,
  VKR_UI_ICON_COUNT,
} VkrUiIcon;

/** Shared placement, style, and text settings for leaf widgets.
 * Labels, buttons, checkboxes and text fields use content plus padding and
 * borders, bounded by style min/max sizes and available space. STRETCH places
 * these text widgets at START without enlarging them; set min_size_pt to
 * reserve more space. Icons and field carets are included in their content
 * size. Structural panels and scroll containers retain normal stretch behavior.
 */
typedef struct VkrUiWidgetConfig {
  VkrUiPlacement placement;
  VkrUiStyle style;
  VkrUiTextConfig text;
  /** Optional leading icon on labels/buttons. Empty content centers the icon.
   */
  VkrUiIcon icon;
  float32_t icon_size_pt;
  /** Icon tint; zero alpha uses the text color. */
  Vec4 icon_color;
  bool8_t disabled;
  /** Text fields retain selection and copying while rejecting mutation. */
  bool8_t read_only;
  /** Keep STRETCH placement for a text widget so it fills its grid cell,
   * for example a search field spanning a toolbar column. */
  bool8_t fill;
  /** Center a label's icon and text horizontally; buttons always center. */
  bool8_t center;
  /** Pointer shape while hovered or held; text fields default to an I-beam. */
  VkrWindowCursor cursor;
  /** Borrowed through vkr_ui_end; shown on hover or keyboard focus. */
  String8 tooltip;
} VkrUiWidgetConfig;

typedef struct VkrUiTextEditBuffer {
  uint8_t *data;
  uint32_t length;
  /** Includes space for the trailing NUL maintained by the text field. */
  uint32_t capacity;
} VkrUiTextEditBuffer;

VkrUiPanelConfig vkr_ui_panel_config_default(void);
VkrUiWidgetConfig vkr_ui_widget_config_default(void);

/** Persistent UI system plus one frame's scratch-backed immediate context. */
typedef struct VkrUiSystem {
  VkrDMemory retained_memory;
  VkrAllocator retained_allocator;
  VkrUiRetainedState **retained_buckets;
  uint32_t retained_bucket_capacity;
  uint32_t retained_count;

  VkrAllocator *frame_allocator;
  VkrFontSystem *fonts;
  /** Optional overrides: text without an explicit font uses default_font. */
  VkrFontHandle default_font;
  VkrFontHandle icon_font;
  VkrFontHandle icon_fill_font;
  InputState *input;
  VkrUiFrameNode *frame_nodes;
  uint32_t frame_node_count;
  uint32_t frame_node_capacity;
  uint32_t frame_tooltip;
  uint32_t frame_tooltip_source;
  uint32_t container_stack[VKR_UI_CONTAINER_STACK_CAPACITY];
  uint32_t container_count;
  VkrUiIdStack id_stack;
  VkrUiDrawCommand *frame_commands;
  uint32_t frame_command_count;
  uint32_t frame_command_capacity;
  VkrUiVertex *cached_vertices;
  uint32_t *cached_indices;
  VkrUiDrawBatch *cached_batches;
  uint32_t cached_vertex_count;
  uint32_t cached_index_count;
  uint32_t cached_batch_count;
  uint64_t cached_draw_hash;
  uint64_t frame_draw_hash;
  uint32_t cached_target_width;
  uint32_t cached_target_height;
  VkrUiTileCache tile_cache;
  VkrUiTileFrame tile_frame;
  float32_t dirty_tile_ratio;
  uint32_t dirty_tile_count;
  uint32_t tile_count;

  uint64_t frame_index;
  uint32_t target_width;
  uint32_t target_height;
  float32_t content_scale;
  float64_t delta_time;
  int32_t mouse_x;
  int32_t mouse_y;
  int32_t mouse_wheel;
  bool8_t mouse_captured;
  bool8_t mouse_pressed;
  bool8_t mouse_released;
  bool8_t focus_claimed;
  bool8_t focused_is_text;
  VkrUiId active_id;
  VkrUiId focused_id;
  VkrUiId hot_id;
  uint32_t input_layer;
  uint32_t mouse_input_layer;
  uint32_t keyboard_input_layer;
  bool8_t keyboard_layer_claimed;
  bool8_t keyboard_navigation_enabled;
  VkrUiInputCapture capture;
  Keys repeat_key;
  float64_t repeat_elapsed;
  float64_t repeat_next;
  /** Tooltip hover timing: a tooltip appears after the theme delay, and
   * stays warm briefly so neighbouring controls show theirs at once. */
  VkrUiId tooltip_owner;
  float64_t tooltip_hover_seconds;
  float64_t tooltip_warm_seconds;
  float32_t tooltip_opacity;
  /** Animations still in flight; the caller keeps redrawing while true. */
  bool8_t animating;
  /** Accessibility: snap every transition instead of easing it. */
  bool8_t reduce_motion;
  /** Accessibility: interface zoom multiplied into the window content scale
   * (1 = native). Offscreen targets keep their explicit scale. */
  float32_t user_scale;
  uint32_t user_scale_revision;
  /** Pointer shape requested by this frame's hovered or held widget. */
  VkrWindowCursor cursor;
  /** Accumulated UI time, used for the caret blink. */
  float64_t time_seconds;

  uint32_t offscreen_width;
  uint32_t offscreen_height;
  bool8_t offscreen_enabled;
  float32_t offscreen_content_scale;
  uint32_t offscreen_content_scale_revision;
  uint32_t content_scale_revision;
  bool8_t frame_open;
  bool8_t frame_draw_ready;
  bool8_t frame_draw_pending;
  bool8_t frame_reuses_cached_draw_list;
  bool8_t draw_cache_valid;
  bool8_t draw_capacity_warning_emitted;
  bool8_t tile_build_warning_emitted;
  bool8_t layout_failure_warning_emitted;
  bool8_t initialized;
} VkrUiSystem;

bool8_t vkr_ui_system_init(VkrUiSystem *system, VkrFontSystem *fonts);
void vkr_ui_system_shutdown(VkrUiSystem *system);
void vkr_ui_system_resize(VkrUiSystem *system, uint32_t width, uint32_t height);
void vkr_ui_system_set_offscreen_size(VkrUiSystem *system, bool8_t enabled,
                                      uint32_t width, uint32_t height);
void vkr_ui_system_set_offscreen_content_scale(VkrUiSystem *system,
                                               float32_t content_scale);
#define VKR_UI_USER_SCALE_MIN 0.75f
#define VKR_UI_USER_SCALE_MAX 2.0f

/** Set the interface zoom; the next frame re-lays out every widget. */
void vkr_ui_system_set_user_scale(VkrUiSystem *system, float32_t scale);

/** Borrow the default text font and the regular/fill icon fonts. The owner
 * keeps them acquired until after vkr_ui_system_shutdown. */
void vkr_ui_system_set_fonts(VkrUiSystem *system, VkrFontHandle text,
                             VkrFontHandle icons, VkrFontHandle icons_fill);

/** Begin one immediate UI frame. The root itself is a grid container. */
bool8_t vkr_ui_begin(VkrUiSystem *system, VkrAllocator *scratch,
                     VkrWindow *window, uint32_t target_width,
                     uint32_t target_height, InputState *input,
                     bool8_t mouse_captured, float64_t delta_time,
                     const VkrUiPanelConfig *root_config);

/** Finalize input and tooltips. Frame scratch stays live through draw
 * preparation. */
VkrUiInputCapture vkr_ui_end(VkrUiSystem *system);

bool8_t vkr_ui_push_id_label(VkrUiSystem *system, String8 label);
bool8_t vkr_ui_push_id_u64(VkrUiSystem *system, uint64_t key);
bool8_t vkr_ui_push_id_pointer(VkrUiSystem *system, const void *pointer);
bool8_t vkr_ui_pop_id(VkrUiSystem *system);

/**
 * Register an input-occluding rectangle before building interactive widgets.
 * The highest registered layer under the pointer receives interaction; layer
 * zero is the ordinary UI behind popups and floating windows.
 */
bool8_t vkr_ui_input_layer_register(VkrUiSystem *system, uint32_t layer,
                                    VkrUiRect rect_px);

/** Select the layer assigned to subsequently built interactive widgets. */
bool8_t vkr_ui_input_layer_set(VkrUiSystem *system, uint32_t layer);

/** Select keyboard focus scope before building widgets (for example an open
 * popup). Pointer presses otherwise select the top layer under the pointer. */
bool8_t vkr_ui_keyboard_layer_set(VkrUiSystem *system, uint32_t layer);
/** Frame-local routing: a focused viewport can consume Tab itself. */
void vkr_ui_keyboard_navigation_enabled(VkrUiSystem *system, bool8_t enabled);

bool8_t vkr_ui_panel_begin(VkrUiSystem *system, String8 id_label,
                           const VkrUiPanelConfig *config);
bool8_t vkr_ui_panel_end(VkrUiSystem *system);
void vkr_ui_label(VkrUiSystem *system, String8 id_label, String8 content,
                  const VkrUiWidgetConfig *config);
bool8_t vkr_ui_button(VkrUiSystem *system, String8 id_label, String8 content,
                      const VkrUiWidgetConfig *config);
/** Frame-borrowed texture handle. The owner retains the resource until GPU
 * completion through the ordinary texture resource system. Fits source aspect
 * into the widget rectangle over an alpha checkerboard. */
void vkr_ui_image(VkrUiSystem *system, String8 id_label,
                  VkrUiTextureRef texture, Vec2 source_size,
                  const VkrUiWidgetConfig *config);
/* Noninteractive cubic wire. Control points are in widget-local points;
 * text_color supplies stroke color. Retained damage includes the curve. */
void vkr_ui_bezier(VkrUiSystem *system, String8 id_label, const Vec2 points[4],
                   float32_t width_pt, const VkrUiWidgetConfig *config);
bool8_t vkr_ui_checkbox(VkrUiSystem *system, String8 id_label, String8 content,
                        bool8_t *value, const VkrUiWidgetConfig *config);
bool8_t vkr_ui_slider_f32(VkrUiSystem *system, String8 id_label,
                          float32_t *value, float32_t minimum,
                          float32_t maximum, const VkrUiWidgetConfig *config);
bool8_t vkr_ui_scroll_area_begin(VkrUiSystem *system, String8 id_label,
                                 const VkrUiPanelConfig *config);
/** Set the open scroll area's vertical offset in points. The caller owns
 * virtualization/reveal policy; layout clamps to its declared row extent. */
bool8_t vkr_ui_scroll_area_offset_set(VkrUiSystem *system, float32_t offset_pt);
bool8_t vkr_ui_scroll_area_end(VkrUiSystem *system);
bool8_t vkr_ui_text_field(VkrUiSystem *system, String8 id_label,
                          VkrUiTextEditBuffer *buffer,
                          const VkrUiWidgetConfig *config);

/** Set an authored widget's left/top margins and fixed size in points before
 * draw preparation. Input keeps using the preceding presented bounds. */
bool8_t vkr_ui_widget_set_rect(VkrUiSystem *system, VkrUiId id,
                               VkrUiRect rect_pt);
/* Update a noninteractive wire after the current scene/camera pose is known,
 * before draw preparation. Coordinates remain widget-local points. */
bool8_t vkr_ui_bezier_set_points(VkrUiSystem *system, VkrUiId id,
                                 const Vec2 points[4]);

VkrUiInputCapture vkr_ui_system_capture(const VkrUiSystem *system);

/** Last presented pixel rectangle of a widget; false before its first layout.
 * Popups use it to anchor beneath the control that opened them. */
bool8_t vkr_ui_widget_rect(const VkrUiSystem *system, VkrUiId id,
                           VkrUiRect *out_rect);

/** Place a text field's caret (collapsing its selection) at a byte offset,
 * for callers that replace the buffer, such as autocomplete. */
void vkr_ui_text_field_set_cursor(VkrUiSystem *system, VkrUiId id,
                                  uint32_t offset);

/** Most recent CPU damage result; 1 means every tile needs redraw. */
float32_t vkr_ui_system_dirty_tile_ratio(const VkrUiSystem *system);

/** Resolve layout, damage and commands, then build the packet-facing indexed
 * stream. */
bool8_t vkr_ui_system_prepare_draw_list(VkrUiSystem *system,
                                        VkrAllocator *frame_allocator,
                                        uint32_t target_width,
                                        uint32_t target_height,
                                        VkrPreparedUiDrawList *out_draw_list);
