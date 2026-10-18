#include "renderer/systems/vkr_ui_system.h"

#include "core/logger.h"
#include "core/vkr_window.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_font_system.h"
#include "vkr_color_transfer.h"
#include "vkr_frame_input.h"

typedef enum VkrUiNodeKind {
  VKR_UI_NODE_ROOT = 0,
  VKR_UI_NODE_PANEL,
  VKR_UI_NODE_LABEL,
  VKR_UI_NODE_BUTTON,
  VKR_UI_NODE_CHECKBOX,
  VKR_UI_NODE_SLIDER,
  VKR_UI_NODE_SCROLL,
  VKR_UI_NODE_TEXT_FIELD,
  VKR_UI_NODE_IMAGE,
  VKR_UI_NODE_BEZIER,
  VKR_UI_NODE_CODE,
  VKR_UI_NODE_PLOT,
  VKR_UI_NODE_PATH,
} VkrUiNodeKind;

/* A path's frame-allocated copy of the caller's points, colors and palette,
   in pixels from the content box. A point the caller gave as non-finite has
   color VKR_UI_PATH_BREAK. */
#define VKR_UI_PATH_BREAK UINT8_MAX
typedef struct VkrUiPathData {
  Vec2 *points;
  uint8_t *colors;
  uint32_t count;
  Vec4 palette[VKR_UI_PATH_PALETTE_CAPACITY];
  uint32_t palette_count;
  float32_t width_px;
  uint64_t hash;
} VkrUiPathData;

/* A plot's frame-allocated copy of the caller's data with finite values and
 * authored colors. Drawing resolves after the caller's arrays may be gone. */
typedef struct VkrUiPlotData {
  uint32_t sample_count;
  float32_t y_min;
  float32_t y_max;
  /* bar_count rows of sample_count finite values. */
  float32_t *bars;
  uint32_t bar_count;
  Vec4 bar_series_colors[VKR_UI_PLOT_BAR_SERIES_CAPACITY];
  /* Per-sample palette index of the first bar series, or NULL. */
  uint8_t *bar_colors;
  Vec4 palette[VKR_UI_PLOT_PALETTE_CAPACITY];
  uint32_t palette_count;
  /* Each sample's stack height (the first line's value without bars), which
   * picks the tallest sample of a column. */
  float32_t *totals;
  /* line_count rows of sample_count finite values. */
  float32_t *lines;
  uint32_t line_count;
  Vec4 line_colors[VKR_UI_PLOT_LINE_SERIES_CAPACITY];
  Vec2 line_ranges[VKR_UI_PLOT_LINE_SERIES_CAPACITY];
  float32_t line_width_pt;
  VkrUiPlotReference references[VKR_UI_PLOT_REFERENCE_CAPACITY];
  uint32_t reference_count;
  /* Covers every field above, which the batched runs draw; their tile
   * fingerprints use it. The marks below are hashed with the node. */
  uint64_t hash;
  uint32_t cursor;
  Vec4 cursor_color;
  uint32_t range_first;
  uint32_t range_last;
  Vec4 range_color;
  uint32_t hovered;
  Vec4 hover_color;
} VkrUiPlotData;

struct VkrUiRetainedState {
  VkrUiId id;
  VkrUiNodeKind kind;
  uint64_t last_seen_frame;
  uint32_t frame_node_index;
  uint64_t build_hash;
  VkrUiRect last_rect;
  VkrUiRect last_clip;
  VkrUiRect last_draw_aabb;
  VkrUiText text;
  Vec2 scroll_offset;
  /* Scroll areas: resolved content height for the scrollbar thumb. */
  float32_t content_extent;
  uint32_t text_cursor;
  uint32_t text_selection;
  /* Eased 0..1 interaction states; see vkr_ui_animate. */
  float32_t hover_t;
  float32_t active_t;
  float32_t focus_t;
  bool8_t text_live;
  bool8_t text_dragging;
  /* Text fields: the last build was read-only; the last press, for double
     and triple clicks. */
  bool8_t text_read_only;
  uint8_t text_clicks;
  float64_t text_click_time;
  /* Scroll areas: the thumb is held, grabbed this far below its top edge. */
  bool8_t scroll_dragging;
  float32_t scroll_grab_offset;
  /* Scroll areas: the scrollbar or a scroll key moved the offset this frame,
   * so it outranks a caller-owned offset; see vkr_ui_scroll_area_offset. */
  bool8_t scroll_moved;
  /* Plots: the held press's pointer x in pixels and column, and whether it
   * has moved far enough to brush. */
  float32_t plot_press_x;
  uint32_t plot_press_column;
  bool8_t plot_dragging;
};

struct VkrUiFrameNode {
  VkrUiId id;
  VkrUiNodeKind kind;
  VkrUiRetainedState *retained;
  uint32_t parent;
  uint32_t first_child;
  uint32_t last_child;
  uint32_t next_sibling;
  VkrUiPlacement placement;
  VkrUiResolvedStyle style;
  const VkrUiTrack *columns;
  uint32_t column_count;
  const VkrUiTrack *rows;
  uint32_t row_count;
  String8 content;
  String8 tooltip;
  VkrUiIcon icon;
  Vec4 icon_color;
  bool8_t leading;
  VkrUiIcon trailing_icon;
  float32_t trailing_icon_size_px;
  VkrUiTextureRef image;
  Vec2 image_size;
  Vec2 bezier_points[4];
  float32_t bezier_width;
  /* Code views: a frame-allocated copy of the caller's view and lines. */
  VkrUiCodeView code;
  uint64_t code_hash;
  uint32_t code_glyphs;
  /* Plots: frame-allocated, NULL for an invalid plot. */
  VkrUiPlotData *plot;
  /* Paths: frame-allocated, NULL for none. */
  VkrUiPathData *path;
  float32_t icon_size_px;
  bool8_t disabled;
  bool8_t focusable;
  bool8_t fill;
  bool8_t center;
  VkrWindowCursor cursor;
  uint32_t input_layer;
  VkrUiRect rect;
  VkrUiRect clip;
  Vec2 intrinsic_size;
  uint64_t build_hash;
  uint32_t draw_first_command;
  uint32_t draw_command_count;
  /* One past the last command of the node and its descendants. */
  uint32_t draw_end_command;
  VkrUiPresence presence;
  float32_t slider_fraction;
  bool8_t checked;
  bool8_t hovered;
  bool8_t active;
  bool8_t clip_children;
};

/* Space between text and a trailing icon such as a dropdown caret. */
#define VKR_UI_TRAILING_GAP_PT 6.0f
#define VKR_UI_NODE_NONE UINT32_MAX
#define VKR_UI_RETAINED_TOMBSTONE ((VkrUiRetainedState *)(uintptr_t)1u)
#define VKR_UI_KEY_REPEAT_DELAY_SECONDS 0.4
#define VKR_UI_KEY_REPEAT_INTERVAL_SECONDS 0.05

vkr_global const VkrUiTrack VKR_UI_ONE_FR_TRACK = {
    .value = 1.0f,
    .unit = VKR_UI_TRACK_FR,
};

vkr_internal const VkrUiTrack *vkr_ui_copy_tracks(VkrUiSystem *system,
                                                  const VkrUiTrack *tracks,
                                                  uint32_t count) {
  if (!tracks || count == 0u)
    return &VKR_UI_ONE_FR_TRACK;
  if (count > VKR_UI_FRAME_NODE_CAPACITY)
    return NULL;
  VkrUiTrack *copy = vkr_allocator_alloc(system->frame_allocator,
                                         (uint64_t)count * sizeof(*copy),
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (copy)
    MemCopy(copy, tracks, (uint64_t)count * sizeof(*copy));
  return copy;
}

vkr_internal bool8_t vkr_ui_rect_equal(VkrUiRect a, VkrUiRect b) {
  return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

vkr_internal bool8_t vkr_ui_point_in_rect(int32_t x, int32_t y,
                                          VkrUiRect rect) {
  return (float32_t)x >= rect.x && (float32_t)x < rect.x + rect.width &&
         (float32_t)y >= rect.y && (float32_t)y < rect.y + rect.height;
}

vkr_internal bool8_t vkr_ui_color_visible(Vec4 color) { return color.w > 0.0f; }

vkr_internal Vec2 vkr_ui_style_clamp_size(Vec2 size,
                                          const VkrUiResolvedStyle *style) {
  size.x = Max(size.x, style->min_size_px.x);
  size.y = Max(size.y, style->min_size_px.y);
  if (style->max_size_px.x > 0.0f)
    size.x = Min(size.x, style->max_size_px.x);
  if (style->max_size_px.y > 0.0f)
    size.y = Min(size.y, style->max_size_px.y);
  return size;
}

vkr_internal VkrUiEdges vkr_ui_edges_add(VkrUiEdges a, VkrUiEdges b) {
  return (VkrUiEdges){
      .top = a.top + b.top,
      .right = a.right + b.right,
      .bottom = a.bottom + b.bottom,
      .left = a.left + b.left,
  };
}

vkr_internal VkrUiPlacement
vkr_ui_normalize_placement(VkrUiPlacement placement) {
  if (placement.column_span == 0u)
    placement.column_span = 1u;
  if (placement.row_span == 0u)
    placement.row_span = 1u;
  if (placement.justify >= VKR_UI_ALIGN_COUNT)
    placement.justify = VKR_UI_ALIGN_STRETCH;
  if (placement.align >= VKR_UI_ALIGN_COUNT)
    placement.align = VKR_UI_ALIGN_STRETCH;
  return placement;
}

VkrUiPanelConfig vkr_ui_panel_config_default(void) {
  return (VkrUiPanelConfig){
      .placement = VKR_UI_PLACEMENT_DEFAULT,
      .columns = &VKR_UI_ONE_FR_TRACK,
      .column_count = 1u,
      .rows = &VKR_UI_ONE_FR_TRACK,
      .row_count = 1u,
      .style = vkr_ui_style_default(),
  };
}

VkrUiWidgetConfig vkr_ui_widget_config_default(void) {
  return (VkrUiWidgetConfig){
      .placement = VKR_UI_PLACEMENT_DEFAULT,
      .style = vkr_ui_style_default(),
      .text = VKR_UI_TEXT_CONFIG_DEFAULT,
      .icon_size_pt = 14.0f,
  };
}

vkr_internal bool8_t vkr_ui_system_dimensions(VkrWindow *window,
                                              VkrUiSystem *system,
                                              uint32_t width, uint32_t height,
                                              uint32_t *out_width,
                                              uint32_t *out_height) {
  if ((width == 0u || height == 0u) && window) {
    const VkrWindowPixelSize size = vkr_window_get_pixel_size(window);
    width = size.width;
    height = size.height;
  }
  if (system->offscreen_enabled && system->offscreen_width > 0u &&
      system->offscreen_height > 0u) {
    width = system->offscreen_width;
    height = system->offscreen_height;
  }
  *out_width = width;
  *out_height = height;
  return width > 0u && height > 0u;
}

vkr_internal VkrWindowContentScale
vkr_ui_system_content_scale(VkrWindow *window, const VkrUiSystem *system) {
  if (system->offscreen_enabled)
    return (VkrWindowContentScale){
        .value = system->offscreen_content_scale,
        .revision = system->offscreen_content_scale_revision,
    };
  VkrWindowContentScale scale = {.value = 1.0f, .revision = 1u};
  if (window)
    scale = vkr_window_get_content_scale(window);
  const float32_t user = system->user_scale > 0.0f ? system->user_scale : 1.0f;
  scale.value *= user;
  scale.revision += system->user_scale_revision << 16u;
  return scale;
}

void vkr_ui_system_set_user_scale(VkrUiSystem *system, float32_t scale) {
  if (!system || !vkr_is_finite_f32(scale))
    return;
  scale = vkr_clamp_f32(scale, VKR_UI_USER_SCALE_MIN, VKR_UI_USER_SCALE_MAX);
  if (scale == system->user_scale)
    return;
  system->user_scale = scale;
  system->user_scale_revision++;
}

vkr_internal uint32_t vkr_ui_retained_bucket(const VkrUiSystem *system,
                                             VkrUiId id) {
  return (uint32_t)id & (system->retained_bucket_capacity - 1u);
}

vkr_internal VkrUiRetainedState *vkr_ui_retained_insert(VkrUiSystem *system,
                                                        uint32_t bucket,
                                                        VkrUiId id,
                                                        VkrUiNodeKind kind) {
  VkrUiRetainedState *state =
      vkr_allocator_alloc(&system->retained_allocator, sizeof(*state),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!state)
    return NULL;
  MemZero(state, sizeof(*state));
  state->id = id;
  state->kind = kind;
  system->retained_buckets[bucket] = state;
  system->retained_count++;
  return state;
}

vkr_internal void vkr_ui_retained_reset(VkrUiRetainedState *state,
                                        VkrUiNodeKind kind) {
  if (state->text_live)
    vkr_ui_text_destroy(&state->text);
  const VkrUiId id = state->id;
  const uint64_t last_seen = state->last_seen_frame;
  MemZero(state, sizeof(*state));
  state->id = id;
  state->kind = kind;
  state->last_seen_frame = last_seen;
}

vkr_internal VkrUiRetainedState *
vkr_ui_retained_get(VkrUiSystem *system, VkrUiId id, VkrUiNodeKind kind) {
  const uint32_t mask = system->retained_bucket_capacity - 1u;
  uint32_t bucket = vkr_ui_retained_bucket(system, id);
  uint32_t first_tombstone = VKR_UI_NODE_NONE;
  for (uint32_t probe = 0u; probe < system->retained_bucket_capacity; ++probe) {
    VkrUiRetainedState *state = system->retained_buckets[bucket];
    if (!state) {
      const uint32_t destination =
          first_tombstone == VKR_UI_NODE_NONE ? bucket : first_tombstone;
      return vkr_ui_retained_insert(system, destination, id, kind);
    }
    if (state == VKR_UI_RETAINED_TOMBSTONE) {
      if (first_tombstone == VKR_UI_NODE_NONE)
        first_tombstone = bucket;
    } else if (state->id == id) {
      if (state->kind != kind)
        vkr_ui_retained_reset(state, kind);
      return state;
    }
    bucket = (bucket + 1u) & mask;
  }
  return first_tombstone == VKR_UI_NODE_NONE
             ? NULL
             : vkr_ui_retained_insert(system, first_tombstone, id, kind);
}

vkr_internal VkrUiRetainedState *vkr_ui_retained_find(const VkrUiSystem *system,
                                                      VkrUiId id) {
  const uint32_t mask = system->retained_bucket_capacity - 1u;
  uint32_t bucket = vkr_ui_retained_bucket(system, id);
  for (uint32_t probe = 0u; probe < system->retained_bucket_capacity; ++probe) {
    VkrUiRetainedState *state = system->retained_buckets[bucket];
    if (!state)
      return NULL;
    if (state != VKR_UI_RETAINED_TOMBSTONE && state->id == id)
      return state;
    bucket = (bucket + 1u) & mask;
  }
  return NULL;
}

vkr_internal void vkr_ui_retained_reclaim(VkrUiSystem *system) {
  for (uint32_t i = 0u; i < system->retained_bucket_capacity; ++i) {
    VkrUiRetainedState *state = system->retained_buckets[i];
    if (!state || state == VKR_UI_RETAINED_TOMBSTONE ||
        system->frame_index - state->last_seen_frame <=
            VKR_UI_RETAINED_GRACE_FRAMES)
      continue;
    if (state->text_live)
      vkr_ui_text_destroy(&state->text);
    if (system->active_id == state->id)
      system->active_id = VKR_UI_ID_NONE;
    if (system->focused_id == state->id)
      system->focused_id = VKR_UI_ID_NONE;
    vkr_allocator_free(&system->retained_allocator, state, sizeof(*state),
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    system->retained_buckets[i] = VKR_UI_RETAINED_TOMBSTONE;
    system->retained_count--;
  }
}

bool8_t vkr_ui_system_init(VkrUiSystem *system, VkrFontSystem *fonts) {
  if (!fonts || !system)
    return false_v;
  MemZero(system, sizeof(*system));
  system->user_scale = 1.0f;
  if (!vkr_dmemory_create(MB(2), MB(64), &system->retained_memory))
    return false_v;
  system->retained_allocator.ctx = &system->retained_memory;
  vkr_dmemory_allocator_create(&system->retained_allocator);
  system->retained_bucket_capacity = VKR_UI_RETAINED_BUCKET_CAPACITY;
  system->retained_buckets =
      vkr_allocator_alloc(&system->retained_allocator,
                          (uint64_t)system->retained_bucket_capacity *
                              sizeof(*system->retained_buckets),
                          VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);
  if (!system->retained_buckets) {
    vkr_dmemory_allocator_destroy(&system->retained_allocator);
    MemZero(system, sizeof(*system));
    return false_v;
  }
  MemZero(system->retained_buckets, (uint64_t)system->retained_bucket_capacity *
                                        sizeof(*system->retained_buckets));
  system->cached_vertices = vkr_allocator_alloc(
      &system->retained_allocator,
      (uint64_t)VKR_UI_VERTEX_CAPACITY * sizeof(*system->cached_vertices),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  system->cached_indices = vkr_allocator_alloc(
      &system->retained_allocator,
      (uint64_t)VKR_UI_INDEX_CAPACITY * sizeof(*system->cached_indices),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  system->cached_batches = vkr_allocator_alloc(
      &system->retained_allocator,
      (uint64_t)VKR_UI_BATCH_CAPACITY * sizeof(*system->cached_batches),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!system->cached_vertices || !system->cached_indices ||
      !system->cached_batches) {
    if (system->cached_vertices)
      vkr_allocator_free(&system->retained_allocator, system->cached_vertices,
                         (uint64_t)VKR_UI_VERTEX_CAPACITY *
                             sizeof(*system->cached_vertices),
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (system->cached_indices)
      vkr_allocator_free(&system->retained_allocator, system->cached_indices,
                         (uint64_t)VKR_UI_INDEX_CAPACITY *
                             sizeof(*system->cached_indices),
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (system->cached_batches)
      vkr_allocator_free(&system->retained_allocator, system->cached_batches,
                         (uint64_t)VKR_UI_BATCH_CAPACITY *
                             sizeof(*system->cached_batches),
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_allocator_free(&system->retained_allocator, system->retained_buckets,
                       (uint64_t)system->retained_bucket_capacity *
                           sizeof(*system->retained_buckets),
                       VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);
    vkr_dmemory_allocator_destroy(&system->retained_allocator);
    MemZero(system, sizeof(*system));
    return false_v;
  }
  system->fonts = fonts;
  system->offscreen_content_scale = 1.0f;
  system->offscreen_content_scale_revision = 1u;
  system->content_scale = 1.0f;
  system->repeat_key = KEY_MAX_KEYS;
  vkr_ui_tile_cache_init(&system->tile_cache, &system->retained_allocator);
  system->initialized = true_v;
  return true_v;
}

void vkr_ui_system_shutdown(VkrUiSystem *system) {
  if (!system || !system->initialized)
    return;
  vkr_ui_tile_cache_destroy(&system->tile_cache);
  for (uint32_t i = 0u; i < ArrayCount(system->presences); ++i) {
    VkrUiPresenceState *presence = &system->presences[i];
    if (presence->commands)
      vkr_allocator_free(&system->retained_allocator, presence->commands,
                         (uint64_t)presence->command_capacity *
                             sizeof(*presence->commands),
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (presence->run_data) {
      vkr_allocator_free(&system->retained_allocator, presence->run_data,
                         presence->run_data_capacity,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
  }
  for (uint32_t i = 0u; i < system->retained_bucket_capacity; ++i) {
    VkrUiRetainedState *state = system->retained_buckets[i];
    if (!state || state == VKR_UI_RETAINED_TOMBSTONE)
      continue;
    if (state->text_live)
      vkr_ui_text_destroy(&state->text);
    vkr_allocator_free(&system->retained_allocator, state, sizeof(*state),
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  }
  vkr_allocator_free(&system->retained_allocator, system->cached_vertices,
                     (uint64_t)VKR_UI_VERTEX_CAPACITY *
                         sizeof(*system->cached_vertices),
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  vkr_allocator_free(&system->retained_allocator, system->cached_indices,
                     (uint64_t)VKR_UI_INDEX_CAPACITY *
                         sizeof(*system->cached_indices),
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  vkr_allocator_free(&system->retained_allocator, system->cached_batches,
                     (uint64_t)VKR_UI_BATCH_CAPACITY *
                         sizeof(*system->cached_batches),
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  vkr_allocator_free(&system->retained_allocator, system->retained_buckets,
                     (uint64_t)system->retained_bucket_capacity *
                         sizeof(*system->retained_buckets),
                     VKR_ALLOCATOR_MEMORY_TAG_HASH_TABLE);
  vkr_dmemory_allocator_destroy(&system->retained_allocator);
  MemZero(system, sizeof(*system));
}

vkr_internal void vkr_ui_system_invalidate_layout(VkrUiSystem *system) {
  system->draw_cache_valid = false_v;
  for (uint32_t i = 0u; i < system->retained_bucket_capacity; ++i) {
    VkrUiRetainedState *state = system->retained_buckets[i];
    if (state && state != VKR_UI_RETAINED_TOMBSTONE)
      state->build_hash = 0u;
  }
}

void vkr_ui_system_resize(VkrUiSystem *system, uint32_t width,
                          uint32_t height) {
  if (!system)
    return;
  if (!system->offscreen_enabled) {
    system->target_width = width;
    system->target_height = height;
  }
  vkr_ui_system_invalidate_layout(system);
}

void vkr_ui_system_set_offscreen_size(VkrUiSystem *system, bool8_t enabled,
                                      uint32_t width, uint32_t height) {
  if (!system)
    return;
  if (system->offscreen_enabled == enabled &&
      system->offscreen_width == width && system->offscreen_height == height)
    return;
  system->offscreen_enabled = enabled;
  system->offscreen_width = width;
  system->offscreen_height = height;
  vkr_ui_system_invalidate_layout(system);
}

void vkr_ui_system_set_offscreen_content_scale(VkrUiSystem *system,
                                               float32_t content_scale) {
  if (!system || !vkr_is_finite_f32(content_scale) || content_scale <= 0.0f ||
      system->offscreen_content_scale == content_scale)
    return;
  system->offscreen_content_scale = content_scale;
  system->offscreen_content_scale_revision++;
  if (system->offscreen_content_scale_revision == 0u)
    system->offscreen_content_scale_revision = 1u;
  vkr_ui_system_invalidate_layout(system);
}

vkr_internal bool8_t vkr_ui_resolve_style(VkrUiSystem *system,
                                          const VkrUiStyle *style,
                                          VkrUiResolvedStyle *out_style) {
  if (vkr_ui_style_resolve(style, system->content_scale, out_style))
    return true_v;
  const VkrUiStyle fallback = vkr_ui_style_default();
  return vkr_ui_style_resolve(&fallback, system->content_scale, out_style);
}

vkr_internal uint32_t vkr_ui_add_node(VkrUiSystem *system, VkrUiId id,
                                      VkrUiNodeKind kind,
                                      VkrUiPlacement placement,
                                      const VkrUiStyle *style) {
  if (!system->frame_open ||
      system->frame_node_count == system->frame_node_capacity)
    return VKR_UI_NODE_NONE;
  VkrUiRetainedState *retained = vkr_ui_retained_get(system, id, kind);
  if (!retained)
    return VKR_UI_NODE_NONE;
  if (retained->last_seen_frame == system->frame_index)
    return VKR_UI_NODE_NONE;
  retained->last_seen_frame = system->frame_index;
  const uint32_t index = system->frame_node_count++;
  retained->frame_node_index = index;
  VkrUiFrameNode *node = &system->frame_nodes[index];
  *node = (VkrUiFrameNode){
      .id = id,
      .kind = kind,
      .retained = retained,
      .parent = VKR_UI_NODE_NONE,
      .first_child = VKR_UI_NODE_NONE,
      .last_child = VKR_UI_NODE_NONE,
      .next_sibling = VKR_UI_NODE_NONE,
      .placement = vkr_ui_normalize_placement(placement),
  };
  if (!vkr_ui_resolve_style(system, style, &node->style))
    return VKR_UI_NODE_NONE;
  if (system->container_count > 0u) {
    node->parent = system->container_stack[system->container_count - 1u];
    VkrUiFrameNode *parent = &system->frame_nodes[node->parent];
    if (parent->first_child == VKR_UI_NODE_NONE)
      parent->first_child = index;
    else
      system->frame_nodes[parent->last_child].next_sibling = index;
    parent->last_child = index;
  }
  return index;
}

vkr_internal float32_t vkr_ui_text_line_height(const VkrUiText *text) {
  if (text->layout.line_count) {
    return text->bounds.size.y / text->layout.line_count;
  }
  const float32_t font_size = text->config.font_size > 0.0f
                                  ? text->config.font_size
                                  : (float32_t)text->resolved_font->size;
  VkrTextStyle style = vkr_text_style_new(
      text->config.font, font_size * text->content_scale, text->config.color);
  style = vkr_text_style_with_font_data(&style, text->resolved_font);
  const VkrText line = vkr_text_from_view(string8_lit(" "), &style);
  return vkr_text_measure(&line).size.y;
}

vkr_internal bool8_t vkr_ui_text_prepare(VkrUiSystem *system,
                                         VkrUiFrameNode *node, String8 content,
                                         const VkrUiTextConfig *source_config) {
  VkrUiRetainedState *retained = node->retained;
  VkrUiTextConfig config = source_config
                               ? *source_config
                               : (VkrUiTextConfig)VKR_UI_TEXT_CONFIG_DEFAULT;
  config.color = node->style.text_color;
  if (config.font_size <= 0.0f && node->style.font_size_px > 0.0f)
    config.font_size = node->style.font_size_px / system->content_scale;
  if (config.font.id == 0u && system->default_font.id != 0u)
    config.font = system->default_font;
  if (!retained->text_live) {
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    if (!vkr_ui_text_create(&system->retained_allocator, system->fonts, content,
                            &config, &retained->text, &error))
      return false_v;
    retained->text_live = true_v;
  } else {
    if (!string8_equals(&retained->text.content, &content) &&
        !vkr_ui_text_set_content(&retained->text, content))
      return false_v;
    vkr_ui_text_set_config(&retained->text, &config);
  }
  vkr_ui_text_set_content_scale(&retained->text, system->content_scale);
  const bool8_t has_geometry = vkr_ui_text_prepare_geometry(&retained->text);
  if (!has_geometry && content.length > 0u)
    return false_v;
  node->content = retained->text.content;
  VkrTextBounds bounds = vkr_ui_text_get_bounds(&retained->text);
  if (node->kind == VKR_UI_NODE_TEXT_FIELD) {
    bounds.size.x += Max(1.0f, system->content_scale);
    bounds.size.y =
        Max(bounds.size.y, vkr_ui_text_line_height(&retained->text));
  }
  node->intrinsic_size = (Vec2){
      bounds.size.x + node->style.padding_px.left +
          node->style.padding_px.right + node->style.border_px.left +
          node->style.border_px.right,
      bounds.size.y + node->style.padding_px.top +
          node->style.padding_px.bottom + node->style.border_px.top +
          node->style.border_px.bottom,
  };
  return true_v;
}

vkr_internal bool8_t vkr_ui_widget_prepare(VkrUiSystem *system,
                                           VkrUiFrameNode *node,
                                           String8 content,
                                           const VkrUiWidgetConfig *config) {
  node->disabled = config->disabled;
  node->tooltip = config->tooltip;
  node->fill = config->fill;
  node->center = config->center;
  node->leading = config->leading;
  node->cursor = config->cursor;
  if (node->disabled)
    node->style.text_color.w *= 0.45f;
  if (!vkr_ui_text_prepare(system, node, content, &config->text))
    return false_v;
  if ((node->kind == VKR_UI_NODE_LABEL || node->kind == VKR_UI_NODE_BUTTON) &&
      config->icon > VKR_UI_ICON_NONE && config->icon < VKR_UI_ICON_COUNT) {
    node->icon = config->icon;
    node->icon_color = config->icon_color.w > 0.0f ? config->icon_color
                                                   : node->style.text_color;
    if (node->disabled && config->icon_color.w > 0.0f)
      node->icon_color.w *= 0.45f;
    node->icon_size_px =
        (vkr_is_finite_f32(config->icon_size_pt) && config->icon_size_pt > 0.0f
             ? config->icon_size_pt
             : 14.0f) *
        system->content_scale;
    const float32_t gap = content.length ? 6.0f * system->content_scale : 0.0f;
    node->intrinsic_size.x += node->icon_size_px + gap;
    node->intrinsic_size.y =
        Max(node->intrinsic_size.y,
            node->icon_size_px + node->style.padding_px.top +
                node->style.padding_px.bottom + node->style.border_px.top +
                node->style.border_px.bottom);
  }
  if ((node->kind == VKR_UI_NODE_LABEL || node->kind == VKR_UI_NODE_BUTTON) &&
      config->trailing_icon > VKR_UI_ICON_NONE &&
      config->trailing_icon < VKR_UI_ICON_COUNT) {
    node->trailing_icon = config->trailing_icon;
    node->trailing_icon_size_px =
        (vkr_is_finite_f32(config->trailing_icon_size_pt) &&
                 config->trailing_icon_size_pt > 0.0f
             ? config->trailing_icon_size_pt
             : 10.0f) *
        system->content_scale;
    node->intrinsic_size.x += node->trailing_icon_size_px +
                              VKR_UI_TRAILING_GAP_PT * system->content_scale;
  }
  if (node->kind != VKR_UI_NODE_CHECKBOX) {
    node->intrinsic_size =
        vkr_ui_style_clamp_size(node->intrinsic_size, &node->style);
  }
  return true_v;
}

vkr_internal bool8_t vkr_ui_key_pressed(VkrUiSystem *system, Keys key) {
  return input_key_just_pressed(system->input, key);
}

vkr_internal bool8_t vkr_ui_shift_down(VkrUiSystem *system) {
  return input_is_key_down(system->input, KEY_SHIFT) ||
         input_is_key_down(system->input, KEY_LSHIFT) ||
         input_is_key_down(system->input, KEY_RSHIFT);
}

vkr_internal bool8_t vkr_ui_keyboard_eligible(const VkrUiSystem *system,
                                              const VkrUiFrameNode *node) {
  return node->focusable && !system->mouse_captured &&
         node->input_layer == system->keyboard_input_layer &&
         vkr_ui_rect_has_area(vkr_ui_rect_intersect(node->retained->last_rect,
                                                    node->retained->last_clip));
}

vkr_internal bool8_t vkr_ui_interact_input(VkrUiSystem *system,
                                           VkrUiFrameNode *node,
                                           bool8_t focusable) {
  VkrUiRetainedState *retained = node->retained;
  node->focusable = focusable && !node->disabled;
  node->input_layer = system->input_layer;
  const bool8_t hovered = !system->mouse_captured &&
                          system->input_layer == system->mouse_input_layer &&
                          vkr_ui_rect_has_area(retained->last_rect) &&
                          vkr_ui_point_in_rect(system->mouse_x, system->mouse_y,
                                               retained->last_rect) &&
                          vkr_ui_point_in_rect(system->mouse_x, system->mouse_y,
                                               retained->last_clip);
  node->hovered = hovered;
  if (hovered) {
    system->hot_id = node->id;
    system->capture.mouse = true_v;
    system->cursor = node->disabled ? VKR_WINDOW_CURSOR_ARROW : node->cursor;
  }
  if (system->active_id == node->id && !node->disabled)
    system->cursor = node->cursor;
  // Scroll containers own hover/wheel handling; descendants own click/drag
  // gestures. Claiming active_id here would starve every child on mouse-down.
  if (node->kind == VKR_UI_NODE_SCROLL)
    return false_v;
  if (node->disabled) {
    if (system->active_id == node->id)
      system->active_id = VKR_UI_ID_NONE;
    if (system->focused_id == node->id)
      system->focused_id = VKR_UI_ID_NONE;
    return false_v;
  }
  int32_t press_x = system->mouse_x, press_y = system->mouse_y;
  if (system->mouse_pressed)
    input_get_button_press_position(system->input, BUTTON_LEFT, &press_x,
                                    &press_y);
  const bool8_t pressed_here =
      system->mouse_pressed && !system->mouse_captured &&
      system->input_layer == system->mouse_input_layer &&
      vkr_ui_rect_has_area(retained->last_rect) &&
      vkr_ui_point_in_rect(press_x, press_y, retained->last_rect) &&
      vkr_ui_point_in_rect(press_x, press_y, retained->last_clip);
  // A fresh press belongs to the last overlapping widget in draw order.
  // Existing drags retain their owner on frames without a new press.
  if (pressed_here) {
    system->active_id = node->id;
    if (focusable) {
      system->focused_id = node->id;
      system->focus_claimed = true_v;
    }
  }
  if (vkr_ui_keyboard_eligible(system, node) &&
      system->focused_id == node->id &&
      (node->kind == VKR_UI_NODE_BUTTON ||
       node->kind == VKR_UI_NODE_CHECKBOX) &&
      (vkr_ui_key_pressed(system, KEY_ENTER) ||
       vkr_ui_key_pressed(system, KEY_SPACE)))
    return true_v;
  node->active = system->active_id == node->id;
  if (node->active)
    system->capture.mouse = true_v;
  if (system->mouse_released && node->active) {
    const bool8_t activated = hovered;
    system->active_id = VKR_UI_ID_NONE;
    node->active = false_v;
    return activated;
  }
  return false_v;
}

/* Exponential approach toward `target`; snaps when close or when motion is
 * reduced, and reports ongoing motion so the caller keeps redrawing. */
vkr_internal float32_t vkr_ui_animate(VkrUiSystem *system, float32_t value,
                                      float32_t target) {
  if (system->reduce_motion)
    return target;
  const float32_t step = 1.0f - vkr_exp_f32(-vkr_ui_theme()->motion_rate *
                                            (float32_t)system->delta_time);
  value += (target - value) * vkr_clamp_f32(step, 0.0f, 1.0f);
  if (vkr_abs_f32(target - value) < 0.004f)
    return target;
  system->animating = true_v;
  return value;
}

vkr_internal void vkr_ui_animate_states(VkrUiSystem *system,
                                        VkrUiFrameNode *node) {
  VkrUiRetainedState *retained = node->retained;
  const bool8_t active = !node->disabled && system->active_id == node->id;
  const bool8_t focused = system->focused_id == node->id;
  retained->hover_t =
      vkr_ui_animate(system, retained->hover_t,
                     node->hovered && !node->disabled ? 1.0f : 0.0f);
  retained->active_t =
      vkr_ui_animate(system, retained->active_t, active ? 1.0f : 0.0f);
  retained->focus_t =
      vkr_ui_animate(system, retained->focus_t, focused ? 1.0f : 0.0f);
}

vkr_internal bool8_t vkr_ui_interact(VkrUiSystem *system, VkrUiFrameNode *node,
                                     bool8_t focusable) {
  const bool8_t activated = vkr_ui_interact_input(system, node, focusable);
  vkr_ui_animate_states(system, node);
  return activated;
}

void vkr_ui_system_set_fonts(VkrUiSystem *system, VkrFontHandle text,
                             VkrFontHandle icons, VkrFontHandle icons_fill) {
  if (!system)
    return;
  system->default_font = text;
  system->icon_font = icons;
  system->icon_fill_font = icons_fill;
  vkr_ui_system_invalidate_layout(system);
}

bool8_t vkr_ui_begin(VkrUiSystem *system, VkrAllocator *scratch,
                     VkrWindow *window, uint32_t target_width,
                     uint32_t target_height, InputState *input,
                     bool8_t mouse_captured, float64_t delta_time,
                     const VkrUiPanelConfig *root_config) {
  if (!scratch || !system || !system->initialized || !input ||
      system->frame_open || !vkr_is_finite_f64(delta_time) || delta_time < 0.0)
    return false_v;
  uint32_t width = 0u;
  uint32_t height = 0u;
  if (!vkr_ui_system_dimensions(window, system, target_width, target_height,
                                &width, &height))
    return false_v;
  VkrWindowContentScale scale = vkr_ui_system_content_scale(window, system);
  if (!vkr_is_finite_f32(scale.value) || scale.value <= 0.0f)
    scale = (VkrWindowContentScale){.value = 1.0f, .revision = 0u};
  if (system->content_scale != scale.value ||
      system->content_scale_revision != scale.revision)
    vkr_ui_system_invalidate_layout(system);

  system->frame_allocator = scratch;
  system->input = input;
  system->target_width = width;
  system->target_height = height;
  system->content_scale = scale.value;
  system->content_scale_revision = scale.revision;
  system->delta_time = delta_time;
  system->time_seconds += delta_time;
  system->animating = false_v;
  system->cursor = VKR_WINDOW_CURSOR_ARROW;
  if (system->repeat_key != KEY_MAX_KEYS) {
    if (input_is_key_up(input, system->repeat_key)) {
      system->repeat_key = KEY_MAX_KEYS;
      system->repeat_elapsed = 0.0;
      system->repeat_next = 0.0;
    } else {
      system->repeat_elapsed += delta_time;
    }
  }
  system->mouse_captured = mouse_captured;
  input_get_mouse_position(input, &system->mouse_x, &system->mouse_y);
  input_get_mouse_wheel(input, &system->mouse_wheel);
  system->mouse_pressed = input_button_just_pressed(input, BUTTON_LEFT);
  system->mouse_released = input_button_just_released(input, BUTTON_LEFT);
  system->focus_claimed = false_v;
  system->focused_is_text = false_v;
  system->hot_id = VKR_UI_ID_NONE;
  system->input_layer = 0u;
  system->mouse_input_layer = 0u;
  system->keyboard_layer_claimed = false_v;
  system->keyboard_navigation_enabled = true_v;
  if (system->mouse_pressed)
    system->keyboard_input_layer = 0u;
  system->capture = (VkrUiInputCapture){0};
  system->frame_commands = NULL;
  system->frame_command_count = 0u;
  system->frame_command_capacity = 0u;
  system->frame_draw_hash = 0u;
  system->frame_draw_ready = false_v;
  system->frame_draw_pending = false_v;
  system->frame_reuses_cached_draw_list = false_v;
  system->frame_index++;
  if (system->frame_index == 0u)
    system->frame_index = 1u;

  system->frame_nodes = vkr_allocator_alloc(
      system->frame_allocator,
      (uint64_t)VKR_UI_FRAME_NODE_CAPACITY * sizeof(*system->frame_nodes),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!system->frame_nodes)
    return false_v;
  system->frame_node_count = 0u;
  system->frame_node_capacity = VKR_UI_FRAME_NODE_CAPACITY;
  system->container_count = 0u;
  vkr_ui_id_stack_init(&system->id_stack);
  system->frame_open = true_v;

  const VkrUiPanelConfig fallback = vkr_ui_panel_config_default();
  const VkrUiPanelConfig *config = root_config ? root_config : &fallback;
  const VkrUiTrack *columns =
      vkr_ui_copy_tracks(system, config->columns, config->column_count);
  const VkrUiTrack *rows =
      vkr_ui_copy_tracks(system, config->rows, config->row_count);
  if (!columns || !rows) {
    system->frame_open = false_v;
    return false_v;
  }
  const uint32_t root = vkr_ui_add_node(
      system, vkr_ui_id_root(), VKR_UI_NODE_ROOT,
      (VkrUiPlacement){.column_span = 1u, .row_span = 1u}, &config->style);
  if (root == VKR_UI_NODE_NONE) {
    system->frame_open = false_v;
    return false_v;
  }
  VkrUiFrameNode *root_node = &system->frame_nodes[root];
  root_node->columns = columns;
  root_node->column_count = config->column_count ? config->column_count : 1u;
  root_node->rows = rows;
  root_node->row_count = config->row_count ? config->row_count : 1u;
  root_node->clip_children = true_v;
  system->container_stack[system->container_count++] = root;
  return true_v;
}

bool8_t vkr_ui_push_id_label(VkrUiSystem *system, String8 label) {
  return system && system->frame_open &&
         vkr_ui_id_stack_push_label(&system->id_stack, label);
}

bool8_t vkr_ui_push_id_u64(VkrUiSystem *system, uint64_t key) {
  return system && system->frame_open &&
         vkr_ui_id_stack_push_u64(&system->id_stack, key);
}

bool8_t vkr_ui_push_id_pointer(VkrUiSystem *system, const void *pointer) {
  return system && system->frame_open &&
         vkr_ui_id_stack_push_pointer(&system->id_stack, pointer);
}

bool8_t vkr_ui_pop_id(VkrUiSystem *system) {
  return system && system->frame_open && vkr_ui_id_stack_pop(&system->id_stack);
}

bool8_t vkr_ui_input_layer_register(VkrUiSystem *system, uint32_t layer,
                                    VkrUiRect rect_px) {
  if (!system || !system->frame_open || layer == 0u ||
      !vkr_ui_rect_has_area(rect_px))
    return false_v;
  /* A captured pointer is virtual; it lies over no layer. */
  if (!system->mouse_captured &&
      vkr_ui_point_in_rect(system->mouse_x, system->mouse_y, rect_px) &&
      layer > system->mouse_input_layer) {
    system->mouse_input_layer = layer;
    if (system->mouse_pressed)
      system->keyboard_input_layer = layer;
    system->capture.mouse = true_v;
  }
  return true_v;
}

bool8_t vkr_ui_input_layer_set(VkrUiSystem *system, uint32_t layer) {
  if (!system || !system->frame_open)
    return false_v;
  system->input_layer = layer;
  return true_v;
}

void vkr_ui_keyboard_navigation_enabled(VkrUiSystem *system, bool8_t enabled) {
  if (system && system->frame_open)
    system->keyboard_navigation_enabled = enabled;
}

bool8_t vkr_ui_keyboard_layer_set(VkrUiSystem *system, uint32_t layer) {
  if (!system || !system->frame_open)
    return false_v;
  system->keyboard_input_layer = layer;
  system->keyboard_layer_claimed = true_v;
  return true_v;
}

bool8_t vkr_ui_panel_begin(VkrUiSystem *system, String8 id_label,
                           const VkrUiPanelConfig *source_config) {
  if (!system || !system->frame_open ||
      system->container_count == VKR_UI_CONTAINER_STACK_CAPACITY)
    return false_v;
  const VkrUiPanelConfig fallback = vkr_ui_panel_config_default();
  const VkrUiPanelConfig *config = source_config ? source_config : &fallback;
  const VkrUiTrack *columns =
      vkr_ui_copy_tracks(system, config->columns, config->column_count);
  const VkrUiTrack *rows =
      vkr_ui_copy_tracks(system, config->rows, config->row_count);
  if (!columns || !rows)
    return false_v;
  const VkrUiId id = vkr_ui_id_stack_widget_label(&system->id_stack, id_label);
  const uint32_t index = vkr_ui_add_node(system, id, VKR_UI_NODE_PANEL,
                                         config->placement, &config->style);
  if (index == VKR_UI_NODE_NONE)
    return false_v;
  VkrUiFrameNode *node = &system->frame_nodes[index];
  node->columns = columns;
  node->column_count = config->column_count ? config->column_count : 1u;
  node->rows = rows;
  node->row_count = config->row_count ? config->row_count : 1u;
  node->clip_children = config->clip_children;
  node->presence = config->presence;
  system->container_stack[system->container_count++] = index;
  return vkr_ui_id_stack_push_label(&system->id_stack, id_label);
}

bool8_t vkr_ui_panel_end(VkrUiSystem *system) {
  if (!system || !system->frame_open || system->container_count <= 1u)
    return false_v;
  system->container_count--;
  return vkr_ui_id_stack_pop(&system->id_stack);
}

void vkr_ui_label(VkrUiSystem *system, String8 id_label, String8 content,
                  const VkrUiWidgetConfig *source_config) {
  if (!system || !system->frame_open)
    return;
  const VkrUiWidgetConfig fallback = vkr_ui_widget_config_default();
  const VkrUiWidgetConfig *config = source_config ? source_config : &fallback;
  const VkrUiId id = vkr_ui_id_stack_widget_label(&system->id_stack, id_label);
  const uint32_t index = vkr_ui_add_node(system, id, VKR_UI_NODE_LABEL,
                                         config->placement, &config->style);
  if (index != VKR_UI_NODE_NONE)
    (void)vkr_ui_widget_prepare(system, &system->frame_nodes[index], content,
                                config);
}

bool8_t vkr_ui_button(VkrUiSystem *system, String8 id_label, String8 content,
                      const VkrUiWidgetConfig *source_config) {
  if (!system || !system->frame_open)
    return false_v;
  VkrUiWidgetConfig fallback = vkr_ui_widget_config_default();
  fallback.style.padding_pt = (VkrUiEdges){6.0f, 10.0f, 6.0f, 10.0f};
  fallback.style.corner_radius_pt = (Vec4){4.0f, 4.0f, 4.0f, 4.0f};
  fallback.style.background_color = (Vec4){0.18f, 0.20f, 0.24f, 1.0f};
  const VkrUiWidgetConfig *config = source_config ? source_config : &fallback;
  const VkrUiId id = vkr_ui_id_stack_widget_label(&system->id_stack, id_label);
  const uint32_t index = vkr_ui_add_node(system, id, VKR_UI_NODE_BUTTON,
                                         config->placement, &config->style);
  if (index == VKR_UI_NODE_NONE ||
      !vkr_ui_widget_prepare(system, &system->frame_nodes[index], content,
                             config))
    return false_v;
  return vkr_ui_interact(system, &system->frame_nodes[index], true_v);
}

void vkr_ui_image(VkrUiSystem *system, String8 id_label,
                  VkrUiTextureRef texture, Vec2 source_size,
                  const VkrUiWidgetConfig *source_config) {
  if (!system || !system->frame_open || !texture.id ||
      !vkr_is_finite_f32(source_size.x) || !vkr_is_finite_f32(source_size.y) ||
      source_size.x <= 0.0f || source_size.y <= 0.0f) {
    return;
  }
  const VkrUiWidgetConfig fallback = vkr_ui_widget_config_default();
  const VkrUiWidgetConfig *config = source_config ? source_config : &fallback;
  const VkrUiId id = vkr_ui_id_stack_widget_label(&system->id_stack, id_label);
  uint32_t index = vkr_ui_add_node(system, id, VKR_UI_NODE_IMAGE,
                                   config->placement, &config->style);
  if (index == VKR_UI_NODE_NONE) {
    return;
  }
  VkrUiFrameNode *node = &system->frame_nodes[index];
  node->image = texture;
  node->image_size = source_size;
  node->intrinsic_size =
      vkr_ui_style_clamp_size((Vec2){source_size.x * system->content_scale,
                                     source_size.y * system->content_scale},
                              &node->style);
}

void vkr_ui_bezier(VkrUiSystem *system, String8 id_label, const Vec2 points[4],
                   float32_t width_pt, const VkrUiWidgetConfig *source_config) {
  if (!system || !system->frame_open || !points ||
      !vkr_is_finite_f32(width_pt) || width_pt <= 0) {
    return;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (!vkr_is_finite_f32(points[i].x) || !vkr_is_finite_f32(points[i].y)) {
      return;
    }
  }
  const VkrUiWidgetConfig fallback = vkr_ui_widget_config_default();
  const VkrUiWidgetConfig *config = source_config ? source_config : &fallback;
  const VkrUiId id = vkr_ui_id_stack_widget_label(&system->id_stack, id_label);
  uint32_t index = vkr_ui_add_node(system, id, VKR_UI_NODE_BEZIER,
                                   config->placement, &config->style);
  if (index == VKR_UI_NODE_NONE) {
    return;
  }
  VkrUiFrameNode *node = &system->frame_nodes[index];
  MemCopy(node->bezier_points, points, sizeof(node->bezier_points));
  node->bezier_width = width_pt;
}

bool8_t vkr_ui_checkbox(VkrUiSystem *system, String8 id_label, String8 content,
                        bool8_t *value,
                        const VkrUiWidgetConfig *source_config) {
  if (!system || !system->frame_open || !value)
    return false_v;
  VkrUiWidgetConfig fallback = vkr_ui_widget_config_default();
  fallback.style.padding_pt = (VkrUiEdges){3.0f, 4.0f, 3.0f, 4.0f};
  const VkrUiWidgetConfig *config = source_config ? source_config : &fallback;
  const VkrUiId id = vkr_ui_id_stack_widget_label(&system->id_stack, id_label);
  const uint32_t index = vkr_ui_add_node(system, id, VKR_UI_NODE_CHECKBOX,
                                         config->placement, &config->style);
  if (index == VKR_UI_NODE_NONE ||
      !vkr_ui_widget_prepare(system, &system->frame_nodes[index], content,
                             config))
    return false_v;
  VkrUiFrameNode *node = &system->frame_nodes[index];
  const float32_t box = 16.0f * system->content_scale;
  node->intrinsic_size.x += box + 6.0f * system->content_scale;
  node->intrinsic_size.y =
      Max(node->intrinsic_size.y,
          box + node->style.padding_px.top + node->style.padding_px.bottom +
              node->style.border_px.top + node->style.border_px.bottom);
  node->intrinsic_size =
      vkr_ui_style_clamp_size(node->intrinsic_size, &node->style);
  const bool8_t changed = vkr_ui_interact(system, node, true_v);
  if (changed)
    *value = !*value;
  node->checked = *value;
  return changed;
}

bool8_t vkr_ui_slider_f32(VkrUiSystem *system, String8 id_label,
                          float32_t *value, float32_t minimum,
                          float32_t maximum,
                          const VkrUiWidgetConfig *source_config) {
  if (!system || !system->frame_open || !value || !vkr_is_finite_f32(*value) ||
      !vkr_is_finite_f32(minimum) || !vkr_is_finite_f32(maximum) ||
      maximum <= minimum)
    return false_v;
  VkrUiWidgetConfig fallback = vkr_ui_widget_config_default();
  fallback.style.min_size_pt = (Vec2){120.0f, 20.0f};
  const VkrUiWidgetConfig *config = source_config ? source_config : &fallback;
  const VkrUiId id = vkr_ui_id_stack_widget_label(&system->id_stack, id_label);
  const uint32_t index = vkr_ui_add_node(system, id, VKR_UI_NODE_SLIDER,
                                         config->placement, &config->style);
  if (index == VKR_UI_NODE_NONE)
    return false_v;
  VkrUiFrameNode *node = &system->frame_nodes[index];
  node->intrinsic_size =
      (Vec2){Max(node->style.min_size_px.x, 120.0f * system->content_scale),
             Max(node->style.min_size_px.y, 20.0f * system->content_scale)};
  node->intrinsic_size =
      vkr_ui_style_clamp_size(node->intrinsic_size, &node->style);
  node->disabled = config->disabled;
  node->tooltip = config->tooltip;
  node->cursor = config->cursor != VKR_WINDOW_CURSOR_ARROW
                     ? config->cursor
                     : VKR_WINDOW_CURSOR_HAND;
  const bool8_t was_active = system->active_id == id;
  const bool8_t released_here = vkr_ui_interact(system, node, true_v);
  bool8_t changed = false_v;
  const bool8_t pointer_edit =
      (system->active_id == id &&
       input_is_button_down(system->input, BUTTON_LEFT)) ||
      released_here || (was_active && system->mouse_released);
  if (pointer_edit && !node->disabled && !system->mouse_captured &&
      node->input_layer == system->mouse_input_layer &&
      node->retained->last_rect.width > 0.0f) {
    const float32_t fraction = vkr_clamp_f32(
        ((float32_t)system->mouse_x - node->retained->last_rect.x) /
            node->retained->last_rect.width,
        0.0f, 1.0f);
    const float32_t next = minimum + fraction * (maximum - minimum);
    changed = next != *value;
    *value = next;
  }
  if (vkr_ui_keyboard_eligible(system, node) && system->focused_id == id) {
    const int32_t direction = (int32_t)vkr_ui_key_pressed(system, KEY_RIGHT) -
                              (int32_t)vkr_ui_key_pressed(system, KEY_LEFT);
    const float32_t next = vkr_clamp_f32(
        *value + (float32_t)direction * (maximum - minimum) * 0.01f, minimum,
        maximum);
    changed |= next != *value;
    *value = next;
  }
  node->slider_fraction =
      vkr_clamp_f32((*value - minimum) / (maximum - minimum), 0.0f, 1.0f);
  return changed;
}

/* Scrollbar geometry shared by drawing and dragging: the thumb and a gutter
 * wider than it for grabbing. False when the content fits. */
vkr_internal bool8_t vkr_ui_scroll_thumb(const VkrUiRetainedState *retained,
                                         const VkrUiResolvedStyle *style,
                                         float32_t scale, VkrUiRect *out_gutter,
                                         VkrUiRect *out_thumb) {
  const VkrUiRect content =
      vkr_ui_style_content_rect(retained->last_rect, style);
  const float32_t viewport = content.height;
  const float32_t extent = retained->content_extent;
  if (viewport <= 0.0f || extent <= viewport + 0.5f)
    return false_v;
  const bool8_t wide = retained->hover_t > 0.0f || retained->scroll_dragging;
  const float32_t thickness = (wide ? 6.0f : 4.0f) * scale;
  const float32_t thumb = Max(24.0f * scale, viewport * viewport / extent);
  const float32_t travel = Max(0.0f, viewport - thumb);
  const float32_t fraction = vkr_clamp_f32(
      retained->scroll_offset.y / Max(1.0f, extent - viewport), 0.0f, 1.0f);
  const float32_t x = retained->last_rect.x + retained->last_rect.width -
                      thickness - 2.0f * scale;
  *out_gutter = (VkrUiRect){x - 4.0f * scale, content.y,
                            thickness + 6.0f * scale, viewport};
  *out_thumb = (VkrUiRect){x, content.y + travel * fraction, thickness, thumb};
  return true_v;
}

/* A press in the gutter pages toward it; a press on the thumb drags it. The
 * gutter consumes that press so rows beneath it do not also activate.
 * Returns whether the scrollbar moved the offset. */
vkr_internal bool8_t vkr_ui_scroll_drag(VkrUiSystem *system,
                                        VkrUiFrameNode *node) {
  VkrUiRetainedState *retained = node->retained;
  VkrUiRect gutter;
  VkrUiRect thumb;
  if (!vkr_ui_scroll_thumb(retained, &node->style, system->content_scale,
                           &gutter, &thumb)) {
    retained->scroll_dragging = false_v;
    return false_v;
  }
  bool8_t paged = false_v;
  const float32_t viewport = gutter.height;
  const float32_t range = retained->content_extent - viewport;
  if (system->mouse_pressed && !system->mouse_captured &&
      node->input_layer == system->mouse_input_layer &&
      vkr_ui_point_in_rect(system->mouse_x, system->mouse_y, gutter) &&
      vkr_ui_point_in_rect(system->mouse_x, system->mouse_y,
                           retained->last_clip)) {
    if (vkr_ui_point_in_rect(system->mouse_x, system->mouse_y, thumb)) {
      retained->scroll_dragging = true_v;
      retained->scroll_grab_offset = (float32_t)system->mouse_y - thumb.y;
    } else {
      const float32_t direction =
          (float32_t)system->mouse_y < thumb.y ? -1.0f : 1.0f;
      retained->scroll_offset.y = vkr_clamp_f32(
          retained->scroll_offset.y + direction * viewport, 0.0f, range);
      paged = true_v;
    }
    system->mouse_pressed = false_v;
    system->capture.mouse = true_v;
  }
  if (!retained->scroll_dragging)
    return paged;
  if (system->mouse_released ||
      !input_is_button_down(system->input, BUTTON_LEFT)) {
    retained->scroll_dragging = false_v;
    return paged;
  }
  const float32_t travel = Max(1.0f, viewport - thumb.height);
  const float32_t top =
      (float32_t)system->mouse_y - retained->scroll_grab_offset - gutter.y;
  retained->scroll_offset.y = vkr_clamp_f32(top / travel, 0.0f, 1.0f) * range;
  system->capture.mouse = true_v;
  return true_v;
}

bool8_t vkr_ui_scroll_area_begin(VkrUiSystem *system, String8 id_label,
                                 const VkrUiPanelConfig *source_config) {
  if (!system || !system->frame_open ||
      system->container_count == VKR_UI_CONTAINER_STACK_CAPACITY)
    return false_v;
  const VkrUiTrack auto_row = {.unit = VKR_UI_TRACK_AUTO};
  VkrUiPanelConfig fallback = vkr_ui_panel_config_default();
  fallback.rows = &auto_row;
  fallback.clip_children = true_v;
  const VkrUiPanelConfig *config = source_config ? source_config : &fallback;
  const VkrUiTrack *columns =
      vkr_ui_copy_tracks(system, config->columns, config->column_count);
  const VkrUiTrack *rows =
      vkr_ui_copy_tracks(system, config->rows, config->row_count);
  if (!columns || !rows)
    return false_v;
  const VkrUiId id = vkr_ui_id_stack_widget_label(&system->id_stack, id_label);
  const uint32_t index = vkr_ui_add_node(system, id, VKR_UI_NODE_SCROLL,
                                         config->placement, &config->style);
  if (index == VKR_UI_NODE_NONE)
    return false_v;
  VkrUiFrameNode *node = &system->frame_nodes[index];
  node->columns = columns;
  node->column_count = config->column_count ? config->column_count : 1u;
  node->rows = rows;
  node->row_count = config->row_count ? config->row_count : 1u;
  node->clip_children = true_v;
  (void)vkr_ui_interact(system, node, true_v);
  node->retained->scroll_moved = vkr_ui_scroll_drag(system, node);
  if (node->hovered && system->mouse_wheel != 0)
    node->retained->scroll_offset.y =
        Max(0.0f,
            node->retained->scroll_offset.y -
                (float32_t)system->mouse_wheel * 32.0f * system->content_scale);
  if (vkr_ui_keyboard_eligible(system, node) && system->focused_id == id) {
    const float32_t page =
        vkr_ui_style_content_rect(node->retained->last_rect, &node->style)
            .height;
    if (vkr_ui_key_pressed(system, KEY_HOME)) {
      node->retained->scroll_offset.y = 0.0f;
      node->retained->scroll_moved = true_v;
    } else if (vkr_ui_key_pressed(system, KEY_END)) {
      // Layout clamps this request to the current declared row extent.
      node->retained->scroll_offset.y = VKR_FLOAT_MAX;
      node->retained->scroll_moved = true_v;
    } else {
      const int32_t direction = (int32_t)vkr_ui_key_pressed(system, KEY_NEXT) -
                                (int32_t)vkr_ui_key_pressed(system, KEY_PRIOR);
      node->retained->scroll_offset.y =
          Max(0.0f, node->retained->scroll_offset.y + direction * page);
      node->retained->scroll_moved |= direction != 0;
    }
  }
  system->container_stack[system->container_count++] = index;
  return vkr_ui_id_stack_push_label(&system->id_stack, id_label);
}

bool8_t vkr_ui_scroll_area_offset(VkrUiSystem *system, float32_t *offset_pt) {
  if (!system || !system->frame_open || !system->container_count ||
      !offset_pt || !vkr_is_finite_f32(*offset_pt) || *offset_pt < 0)
    return false_v;
  VkrUiFrameNode *node =
      &system
           ->frame_nodes[system->container_stack[system->container_count - 1u]];
  if (node->kind != VKR_UI_NODE_SCROLL)
    return false_v;
  VkrUiRetainedState *retained = node->retained;
  if (!retained->scroll_moved) {
    retained->scroll_offset.y = *offset_pt * system->content_scale;
    return true_v;
  }
  /* The caller virtualizes with this offset before layout clamps it. */
  const float32_t viewport =
      vkr_ui_style_content_rect(retained->last_rect, &node->style).height;
  retained->scroll_offset.y =
      Min(retained->scroll_offset.y,
          Max(0.0f, retained->content_extent - viewport));
  *offset_pt = retained->scroll_offset.y / system->content_scale;
  retained->scroll_moved = false_v;
  return true_v;
}

bool8_t vkr_ui_scroll_area_end(VkrUiSystem *system) {
  return vkr_ui_panel_end(system);
}

vkr_internal uint32_t vkr_ui_utf8_previous(const uint8_t *data,
                                           uint32_t cursor) {
  if (cursor == 0u)
    return 0u;
  cursor--;
  while (cursor > 0u && (data[cursor] & 0xc0u) == 0x80u)
    cursor--;
  return cursor;
}

vkr_internal uint32_t vkr_ui_utf8_next(const uint8_t *data, uint32_t length,
                                       uint32_t cursor) {
  if (cursor >= length)
    return length;
  cursor++;
  while (cursor < length && (data[cursor] & 0xc0u) == 0x80u)
    cursor++;
  return cursor;
}

vkr_internal void vkr_ui_text_edit_erase(VkrUiTextEditBuffer *buffer,
                                         uint32_t begin, uint32_t end) {
  if (begin >= end || end > buffer->length)
    return;
  MemCopy(buffer->data + begin, buffer->data + end, buffer->length - end);
  buffer->length -= end - begin;
  if (buffer->length < buffer->capacity)
    buffer->data[buffer->length] = 0u;
}

vkr_internal bool8_t vkr_ui_key_repeat(VkrUiSystem *system, Keys key) {
  if (input_key_just_pressed(system->input, key)) {
    system->repeat_key = key;
    system->repeat_elapsed = 0.0;
    system->repeat_next = VKR_UI_KEY_REPEAT_DELAY_SECONDS;
    return true_v;
  }
  if (input_is_key_up(system->input, key))
    return false_v;
  if (system->repeat_key != key || system->repeat_elapsed < system->repeat_next)
    return false_v;
  do {
    system->repeat_next += VKR_UI_KEY_REPEAT_INTERVAL_SECONDS;
  } while (system->repeat_next <= system->repeat_elapsed);
  return true_v;
}

vkr_internal bool8_t vkr_ui_text_edit_insert(VkrUiTextEditBuffer *buffer,
                                             uint32_t *cursor,
                                             uint32_t *selection,
                                             uint32_t codepoint) {
  /* Line breaks reach here only from multiline fields. */
  if ((codepoint < 0x20u && codepoint != '\n') || codepoint == 0x7fu)
    return false_v;
  uint8_t encoded[4];
  const uint8_t encoded_length =
      vkr_utf8_encode(codepoint, encoded, sizeof(encoded));
  if (encoded_length == 0u)
    return false_v;
  const uint32_t selection_begin = Min(*cursor, *selection);
  const uint32_t selection_end = Max(*cursor, *selection);
  const uint32_t remaining_length =
      buffer->length - (selection_end - selection_begin);
  if (remaining_length + encoded_length >= buffer->capacity)
    return false_v;
  vkr_ui_text_edit_erase(buffer, selection_begin, selection_end);
  MemCopy(buffer->data + selection_begin + encoded_length,
          buffer->data + selection_begin, buffer->length - selection_begin);
  MemCopy(buffer->data + selection_begin, encoded, encoded_length);
  buffer->length += encoded_length;
  buffer->data[buffer->length] = 0u;
  *cursor = selection_begin + encoded_length;
  *selection = *cursor;
  return true_v;
}

vkr_internal Vec2 vkr_ui_text_cursor_position(const VkrUiText *text,
                                              uint32_t byte_offset) {
  Vec2 caret = {0};
  uint32_t glyph = 0u;
  for (uint32_t offset = 0u; offset < text->content.length;) {
    const VkrCodepoint cp = vkr_utf8_decode(text->content.str + offset,
                                            text->content.length - offset);
    if (cp.byte_length == 0u)
      break;
    if (cp.value != '\n' && glyph < text->layout.glyphs.length) {
      const VkrTextGlyph *item = &text->layout.glyphs.data[glyph];
      caret = (Vec2){item->position.x, item->position.y - text->bounds.ascent};
    }
    if (offset >= byte_offset)
      return caret;
    if (cp.value == '\n') {
      caret.x = 0.0f;
      caret.y += vkr_ui_text_line_height(text);
    } else if (glyph < text->layout.glyphs.length) {
      caret.x += text->layout.glyphs.data[glyph++].advance;
    }
    offset += cp.byte_length;
  }
  return caret;
}

vkr_internal uint32_t vkr_ui_text_mouse_cursor(const VkrUiText *text,
                                               Vec2 mouse) {
  uint32_t best = 0u, glyph = 0u;
  float32_t distance = VKR_FLOAT_INFINITY;
  Vec2 caret = {0};
  const float32_t height = vkr_ui_text_line_height(text);
  for (uint32_t offset = 0u; offset <= text->content.length;) {
    VkrCodepoint cp = {0};
    if (offset < text->content.length)
      cp = vkr_utf8_decode(text->content.str + offset,
                           text->content.length - offset);
    if (cp.byte_length && cp.value != '\n' &&
        glyph < text->layout.glyphs.length) {
      const VkrTextGlyph *item = &text->layout.glyphs.data[glyph];
      caret = (Vec2){item->position.x, item->position.y - text->bounds.ascent};
    }
    const float32_t dy = mouse.y < caret.y ? caret.y - mouse.y
                         : mouse.y > caret.y + height
                             ? mouse.y - caret.y - height
                             : 0.0f;
    const float32_t cost = dy * 10000.0f + vkr_abs_f32(mouse.x - caret.x);
    if (cost < distance) {
      distance = cost;
      best = offset;
    }
    if (!cp.byte_length)
      break;
    if (cp.value == '\n') {
      caret.x = 0.0f;
      caret.y += height;
    } else if (glyph < text->layout.glyphs.length)
      caret.x += text->layout.glyphs.data[glyph++].advance;
    offset += cp.byte_length;
  }
  return best;
}

/* Native text-editing conventions: words move with Option (Control
   elsewhere) and lines with Command on macOS. */
vkr_internal bool8_t vkr_ui_word_modifier(uint8_t modifiers) {
#if defined(PLATFORM_APPLE)
  return (modifiers & VKR_INPUT_MOD_ALT) != 0;
#else
  return (modifiers & VKR_INPUT_MOD_CONTROL) != 0;
#endif
}

vkr_internal bool8_t vkr_ui_line_modifier(uint8_t modifiers) {
#if defined(PLATFORM_APPLE)
  return (modifiers & VKR_INPUT_MOD_SUPER) != 0;
#else
  (void)modifiers;
  return false_v;
#endif
}

/* Word bytes: letters, digits, underscore and any non-ASCII byte. */
vkr_internal bool8_t vkr_ui_text_word_byte(uint8_t byte) {
  return byte >= 0x80u || byte == '_' || (byte >= '0' && byte <= '9') ||
         (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

vkr_internal uint32_t vkr_ui_text_word_left(const uint8_t *data,
                                            uint32_t cursor) {
  while (cursor > 0u && !vkr_ui_text_word_byte(data[cursor - 1u]))
    cursor = vkr_ui_utf8_previous(data, cursor);
  while (cursor > 0u && vkr_ui_text_word_byte(data[cursor - 1u]))
    cursor = vkr_ui_utf8_previous(data, cursor);
  return cursor;
}

vkr_internal uint32_t vkr_ui_text_word_right(const uint8_t *data,
                                             uint32_t length, uint32_t cursor) {
  while (cursor < length && !vkr_ui_text_word_byte(data[cursor]))
    cursor = vkr_ui_utf8_next(data, length, cursor);
  while (cursor < length && vkr_ui_text_word_byte(data[cursor]))
    cursor = vkr_ui_utf8_next(data, length, cursor);
  return cursor;
}

vkr_internal uint32_t vkr_ui_text_line_start(const uint8_t *data,
                                             uint32_t cursor) {
  while (cursor > 0u && data[cursor - 1u] != '\n')
    --cursor;
  return cursor;
}

vkr_internal uint32_t vkr_ui_text_line_end(const uint8_t *data, uint32_t length,
                                           uint32_t cursor) {
  while (cursor < length && data[cursor] != '\n')
    ++cursor;
  return cursor;
}

/* Kinds of edit for undo coalescing. */
enum {
  VKR_UI_TEXT_EDIT_OTHER = 0,
  VKR_UI_TEXT_EDIT_TYPE,
  VKR_UI_TEXT_EDIT_ERASE,
};

vkr_internal void vkr_ui_text_snapshot_push(VkrUiTextSnapshot *stack,
                                            uint32_t *count,
                                            const VkrUiTextEditBuffer *buffer,
                                            uint32_t cursor) {
  if (*count == VKR_UI_TEXT_UNDO_DEPTH) {
    MemCopy(stack, stack + 1, sizeof(*stack) * (VKR_UI_TEXT_UNDO_DEPTH - 1u));
    --*count;
  }
  VkrUiTextSnapshot *snapshot = &stack[(*count)++];
  snapshot->length = buffer->length;
  snapshot->cursor = cursor;
  MemCopy(snapshot->data, buffer->data, buffer->length);
}

/* Before an edit: remember the text unless it continues the last edit of
   the same kind at the same caret within a second. */
vkr_internal void vkr_ui_text_checkpoint(VkrUiSystem *system,
                                         const VkrUiTextEditBuffer *buffer,
                                         const VkrUiRetainedState *retained,
                                         uint32_t kind) {
  if (buffer->capacity > VKR_UI_TEXT_UNDO_BYTES)
    return;
  const bool8_t continues = kind != VKR_UI_TEXT_EDIT_OTHER &&
                            kind == system->text_edit_kind &&
                            retained->text_cursor == system->text_edit_cursor &&
                            retained->text_cursor == retained->text_selection &&
                            system->time_seconds - system->text_edit_time < 1.0;
  if (!continues)
    vkr_ui_text_snapshot_push(system->text_undo, &system->text_undo_count,
                              buffer, retained->text_cursor);
  system->text_redo_count = 0u;
  system->text_edit_kind = kind;
  system->text_edit_time = system->time_seconds;
}

/* Undo or redo: the current text moves to the other stack. */
vkr_internal bool8_t vkr_ui_text_history_step(VkrUiSystem *system,
                                              VkrUiTextEditBuffer *buffer,
                                              VkrUiRetainedState *retained,
                                              bool8_t redo) {
  VkrUiTextSnapshot *from = redo ? system->text_redo : system->text_undo;
  uint32_t *from_count =
      redo ? &system->text_redo_count : &system->text_undo_count;
  if (!*from_count || buffer->capacity > VKR_UI_TEXT_UNDO_BYTES)
    return false_v;
  vkr_ui_text_snapshot_push(redo ? system->text_undo : system->text_redo,
                            redo ? &system->text_undo_count
                                 : &system->text_redo_count,
                            buffer, retained->text_cursor);
  const VkrUiTextSnapshot *snapshot = &from[--*from_count];
  MemCopy(buffer->data, snapshot->data, snapshot->length);
  buffer->length = snapshot->length;
  buffer->data[buffer->length] = 0u;
  retained->text_cursor = Min(snapshot->cursor, buffer->length);
  retained->text_selection = retained->text_cursor;
  system->text_edit_kind = VKR_UI_TEXT_EDIT_OTHER;
  return true_v;
}

/* Paste the clipboard over the selection, as typed characters; a multiline
   field keeps line breaks, taking CR LF and CR as one. */
vkr_internal bool8_t vkr_ui_text_paste(VkrUiSystem *system,
                                       VkrUiTextEditBuffer *buffer,
                                       VkrUiRetainedState *retained,
                                       bool8_t multiline) {
  // This bounded paste copy expires with the caller's frame scratch.
  uint8_t *paste =
      vkr_allocator_alloc(system->frame_allocator, buffer->capacity,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t length = 0u;
  if (!paste ||
      !vkr_platform_clipboard_read_text(paste, buffer->capacity, &length) ||
      !length)
    return false_v;
  vkr_ui_text_checkpoint(system, buffer, retained, VKR_UI_TEXT_EDIT_OTHER);
  bool8_t changed = false_v;
  for (uint32_t offset = 0u; offset < length;) {
    const VkrCodepoint cp = vkr_utf8_decode(paste + offset, length - offset);
    if (cp.byte_length == 0u)
      break;
    const bool8_t crlf =
        cp.value == '\r' && offset + 1u < length && paste[offset + 1u] == '\n';
    const uint32_t value = multiline && cp.value == '\r' ? '\n' : cp.value;
    if ((value >= 0x20u && value != 0x7fu) || (multiline && value == '\n')) {
      if (!vkr_ui_text_edit_insert(buffer, &retained->text_cursor,
                                   &retained->text_selection, value))
        break;
      changed = true_v;
    }
    offset += cp.byte_length + (crlf ? 1u : 0u);
  }
  return changed;
}

/* Erase [begin, end) as one undoable step of `kind`. */
vkr_internal bool8_t vkr_ui_text_erase(VkrUiSystem *system,
                                       VkrUiTextEditBuffer *buffer,
                                       VkrUiRetainedState *retained,
                                       uint32_t begin, uint32_t end,
                                       uint32_t kind) {
  if (begin >= end)
    return false_v;
  vkr_ui_text_checkpoint(system, buffer, retained, kind);
  vkr_ui_text_edit_erase(buffer, begin, end);
  retained->text_cursor = begin;
  retained->text_selection = begin;
  return true_v;
}

/* Keyboard and queued-command editing of a focused field. Returns whether the
   text changed; `moved` reports caret moves without edits. */
vkr_internal bool8_t vkr_ui_text_field_keys(
    VkrUiSystem *system, VkrUiFrameNode *node, VkrUiTextEditBuffer *buffer,
    bool8_t read_only, bool8_t multiline, VkrUiTextCommand command,
    bool8_t *out_moved) {
  VkrUiRetainedState *retained = node->retained;
  InputState *input = system->input;
  bool8_t changed = false_v;
  uint32_t selection_begin =
      Min(retained->text_cursor, retained->text_selection);
  uint32_t selection_end = Max(retained->text_cursor, retained->text_selection);
  static const Keys shortcut_keys[] = {KEY_A, KEY_C, KEY_X,
                                       KEY_V, KEY_Z, KEY_Y};
  bool8_t shortcut = false_v;
  for (uint32_t i = 0; i < ArrayCount(shortcut_keys); ++i)
    shortcut |= input_key_shortcut_modifier(input, shortcut_keys[i]);
  const bool8_t shortcut_shift =
      (input_key_press_modifiers(input, KEY_Z) & VKR_INPUT_MOD_SHIFT) != 0;
#define VKR_UI_SHORTCUT(key)                                                   \
  (input_key_shortcut_modifier(input, key) && vkr_ui_key_pressed(system, key))
  if (VKR_UI_SHORTCUT(KEY_Z))
    command =
        shortcut_shift ? VKR_UI_TEXT_COMMAND_REDO : VKR_UI_TEXT_COMMAND_UNDO;
  else if (VKR_UI_SHORTCUT(KEY_Y))
    command = VKR_UI_TEXT_COMMAND_REDO;
  else if (VKR_UI_SHORTCUT(KEY_A))
    command = VKR_UI_TEXT_COMMAND_SELECT_ALL;
  else if (VKR_UI_SHORTCUT(KEY_C))
    command = VKR_UI_TEXT_COMMAND_COPY;
  else if (VKR_UI_SHORTCUT(KEY_X))
    command = VKR_UI_TEXT_COMMAND_CUT;
  else if (VKR_UI_SHORTCUT(KEY_V))
    command = VKR_UI_TEXT_COMMAND_PASTE;
#undef VKR_UI_SHORTCUT
  switch (command) {
  case VKR_UI_TEXT_COMMAND_SELECT_ALL:
    retained->text_selection = 0u;
    retained->text_cursor = buffer->length;
    retained->text_dragging = false_v;
    break;
  case VKR_UI_TEXT_COMMAND_COPY:
  case VKR_UI_TEXT_COMMAND_CUT:
    if (selection_end > selection_begin &&
        vkr_platform_clipboard_write_text(buffer->data + selection_begin,
                                          selection_end - selection_begin) &&
        !read_only && command == VKR_UI_TEXT_COMMAND_CUT)
      changed |= vkr_ui_text_erase(system, buffer, retained, selection_begin,
                                   selection_end, VKR_UI_TEXT_EDIT_OTHER);
    break;
  case VKR_UI_TEXT_COMMAND_PASTE:
    if (!read_only)
      changed |= vkr_ui_text_paste(system, buffer, retained, multiline);
    break;
  case VKR_UI_TEXT_COMMAND_DELETE:
    if (!read_only)
      changed |= vkr_ui_text_erase(system, buffer, retained, selection_begin,
                                   selection_end, VKR_UI_TEXT_EDIT_OTHER);
    break;
  case VKR_UI_TEXT_COMMAND_UNDO:
  case VKR_UI_TEXT_COMMAND_REDO:
    if (!read_only)
      changed |= vkr_ui_text_history_step(system, buffer, retained,
                                          command == VKR_UI_TEXT_COMMAND_REDO);
    break;
  case VKR_UI_TEXT_COMMAND_NONE:
    break;
  }
  if (!read_only && !shortcut) {
    uint32_t character_count = 0u;
    const uint32_t *characters = input_get_characters(input, &character_count);
    for (uint32_t i = 0u; i < character_count; ++i) {
      if (characters[i] < 0x20u || characters[i] == 0x7fu)
        continue;
      vkr_ui_text_checkpoint(system, buffer, retained, VKR_UI_TEXT_EDIT_TYPE);
      if (vkr_ui_text_edit_insert(buffer, &retained->text_cursor,
                                  &retained->text_selection, characters[i])) {
        changed = true_v;
        system->text_edit_cursor = retained->text_cursor;
      }
    }
  }
  if (multiline && !read_only && !shortcut &&
      vkr_ui_key_repeat(system, KEY_ENTER)) {
    vkr_ui_text_checkpoint(system, buffer, retained, VKR_UI_TEXT_EDIT_OTHER);
    if (vkr_ui_text_edit_insert(buffer, &retained->text_cursor,
                                &retained->text_selection, '\n'))
      changed = true_v;
  }
  selection_begin = Min(retained->text_cursor, retained->text_selection);
  selection_end = Max(retained->text_cursor, retained->text_selection);
  const uint8_t *data = buffer->data;
  if (!read_only && vkr_ui_key_repeat(system, KEY_BACKSPACE)) {
    /* Option deletes the previous word, Command back to the line start. */
    const uint8_t modifiers = input_key_press_modifiers(input, KEY_BACKSPACE);
    if (selection_begin == selection_end)
      selection_begin = vkr_ui_line_modifier(modifiers)
                            ? vkr_ui_text_line_start(data, selection_begin)
                        : vkr_ui_word_modifier(modifiers)
                            ? vkr_ui_text_word_left(data, selection_begin)
                            : vkr_ui_utf8_previous(data, selection_begin);
    if (vkr_ui_text_erase(system, buffer, retained, selection_begin,
                          selection_end, VKR_UI_TEXT_EDIT_ERASE)) {
      changed = true_v;
      system->text_edit_cursor = retained->text_cursor;
    }
  } else if (!read_only && vkr_ui_key_repeat(system, KEY_DELETE)) {
    const uint8_t modifiers = input_key_press_modifiers(input, KEY_DELETE);
    if (selection_begin == selection_end)
      selection_end =
          vkr_ui_line_modifier(modifiers)
              ? vkr_ui_text_line_end(data, buffer->length, selection_end)
          : vkr_ui_word_modifier(modifiers)
              ? vkr_ui_text_word_right(data, buffer->length, selection_end)
              : vkr_ui_utf8_next(data, buffer->length, selection_end);
    changed |= vkr_ui_text_erase(system, buffer, retained, selection_begin,
                                 selection_end, VKR_UI_TEXT_EDIT_OTHER);
  }
  selection_begin = Min(retained->text_cursor, retained->text_selection);
  selection_end = Max(retained->text_cursor, retained->text_selection);
  bool8_t shift = vkr_ui_shift_down(system);
  bool8_t moved = false_v;
  const Keys horizontal[2] = {KEY_LEFT, KEY_RIGHT};
  for (uint32_t side = 0u; side < 2u; ++side) {
    if (!vkr_ui_key_repeat(system, horizontal[side]))
      continue;
    const uint8_t modifiers =
        input_key_press_modifiers(input, horizontal[side]);
    shift = (modifiers & VKR_INPUT_MOD_SHIFT) != 0;
    const uint32_t cursor = retained->text_cursor;
    if (vkr_ui_line_modifier(modifiers))
      retained->text_cursor =
          side ? vkr_ui_text_line_end(data, buffer->length, cursor)
               : vkr_ui_text_line_start(data, cursor);
    else if (vkr_ui_word_modifier(modifiers))
      retained->text_cursor =
          side ? vkr_ui_text_word_right(data, buffer->length, cursor)
               : vkr_ui_text_word_left(data, cursor);
    else if (!shift && selection_begin != selection_end)
      retained->text_cursor = side ? selection_end : selection_begin;
    else
      retained->text_cursor =
          side ? vkr_ui_utf8_next(data, buffer->length, cursor)
               : vkr_ui_utf8_previous(data, cursor);
    moved = true_v;
  }
  const int32_t vertical = (int32_t)vkr_ui_key_repeat(system, KEY_DOWN) -
                           (int32_t)vkr_ui_key_repeat(system, KEY_UP);
  if (vertical) {
    const Keys key = vertical > 0 ? KEY_DOWN : KEY_UP;
    const uint8_t modifiers = input_key_press_modifiers(input, key);
    shift = (modifiers & VKR_INPUT_MOD_SHIFT) != 0;
    if (vkr_ui_line_modifier(modifiers) ||
        retained->text.layout.line_count <= 1u) {
      /* Command, or a single line: to the text's start or end. */
      retained->text_cursor = vertical > 0 ? buffer->length : 0u;
    } else {
      Vec2 target =
          vkr_ui_text_cursor_position(&retained->text, retained->text_cursor);
      target.y += ((float32_t)vertical + 0.5f) *
                  vkr_ui_text_line_height(&retained->text);
      retained->text_cursor = vkr_ui_text_mouse_cursor(&retained->text, target);
    }
    moved = true_v;
  }
  if (vkr_ui_key_pressed(system, KEY_HOME)) {
    shift =
        (input_key_press_modifiers(input, KEY_HOME) & VKR_INPUT_MOD_SHIFT) != 0;
    retained->text_cursor = 0u;
    moved = true_v;
  }
  if (vkr_ui_key_pressed(system, KEY_END)) {
    shift =
        (input_key_press_modifiers(input, KEY_END) & VKR_INPUT_MOD_SHIFT) != 0;
    retained->text_cursor = buffer->length;
    moved = true_v;
  }
  if (moved && !shift)
    retained->text_selection = retained->text_cursor;
  *out_moved = moved;
  return changed;
}

/* The word, or run of spaces or punctuation, around a byte offset. */
vkr_internal void vkr_ui_text_select_word(const VkrUiTextEditBuffer *buffer,
                                          VkrUiRetainedState *retained) {
  const uint8_t *data = buffer->data;
  uint32_t at = Min(retained->text_cursor, buffer->length);
  if (at == buffer->length && at > 0u)
    at = vkr_ui_utf8_previous(data, at);
  if (at >= buffer->length)
    return;
  const bool8_t word = vkr_ui_text_word_byte(data[at]);
  uint32_t begin = at;
  uint32_t end = at;
  while (begin > 0u && vkr_ui_text_word_byte(data[begin - 1u]) == word &&
         data[begin - 1u] != '\n')
    begin = vkr_ui_utf8_previous(data, begin);
  while (end < buffer->length && vkr_ui_text_word_byte(data[end]) == word &&
         data[end] != '\n')
    end = vkr_ui_utf8_next(data, buffer->length, end);
  retained->text_selection = begin;
  retained->text_cursor = end;
}

bool8_t vkr_ui_text_field(VkrUiSystem *system, String8 id_label,
                          VkrUiTextEditBuffer *buffer,
                          const VkrUiWidgetConfig *source_config) {
  if (!system || !system->frame_open || !buffer || !buffer->data ||
      buffer->capacity == 0u || buffer->length >= buffer->capacity)
    return false_v;
  buffer->data[buffer->length] = 0u;
  VkrUiWidgetConfig fallback = vkr_ui_widget_config_default();
  fallback.style.padding_pt = (VkrUiEdges){4.0f, 6.0f, 4.0f, 6.0f};
  fallback.style.background_color = (Vec4){0.08f, 0.09f, 0.11f, 1.0f};
  const VkrUiWidgetConfig *config = source_config ? source_config : &fallback;
  const VkrUiId id = vkr_ui_id_stack_widget_label(&system->id_stack, id_label);
  const uint32_t index = vkr_ui_add_node(system, id, VKR_UI_NODE_TEXT_FIELD,
                                         config->placement, &config->style);
  if (index == VKR_UI_NODE_NONE)
    return false_v;
  VkrUiFrameNode *node = &system->frame_nodes[index];
  node->disabled = config->disabled;
  node->tooltip = config->tooltip;
  node->fill = config->fill;
  node->cursor = config->cursor != VKR_WINDOW_CURSOR_ARROW
                     ? config->cursor
                     : VKR_WINDOW_CURSOR_IBEAM;
  if (node->disabled)
    node->style.text_color.w *= 0.45f;
  const String8 incoming_content = {.str = buffer->data,
                                    .length = buffer->length};
  if (config->read_only && node->retained->text_live &&
      !string8_equals(&node->retained->text.content, &incoming_content)) {
    node->retained->text_cursor = 0u;
    node->retained->text_selection = 0u;
    node->retained->scroll_offset = (Vec2){0};
  }
  if (!vkr_ui_text_prepare(system, node, incoming_content, &config->text))
    return false_v;
  (void)vkr_ui_interact(system, node, true_v);
  VkrUiRetainedState *retained = node->retained;
  retained->text_read_only = config->read_only;
  const uint32_t previous_cursor = retained->text_cursor;
  /* A queued command focuses its field. */
  VkrUiTextCommand command = VKR_UI_TEXT_COMMAND_NONE;
  if (system->text_command_id == id && !node->disabled) {
    command = system->text_command;
    system->text_command_id = VKR_UI_ID_NONE;
    system->text_command = VKR_UI_TEXT_COMMAND_NONE;
    system->focused_id = id;
  }
  if (!node->disabled && !system->mouse_captured &&
      node->input_layer == system->keyboard_input_layer) {
    const VkrUiRect box =
        vkr_ui_style_content_rect(retained->last_rect, &node->style);
    if (system->mouse_pressed && system->focused_id == id) {
      int32_t press_x, press_y;
      input_get_button_press_position(system->input, BUTTON_LEFT, &press_x,
                                      &press_y);
      if (vkr_ui_point_in_rect(press_x, press_y, retained->last_rect) &&
          vkr_ui_point_in_rect(press_x, press_y, retained->last_clip)) {
        const Vec2 anchor = {
            (float32_t)press_x - box.x + retained->scroll_offset.x,
            (float32_t)press_y - box.y + retained->scroll_offset.y};
        retained->text_cursor =
            vkr_ui_text_mouse_cursor(&retained->text, anchor);
        /* Presses close together count up: two select a word, three the
           whole text. */
        const bool8_t again =
            system->time_seconds - retained->text_click_time < 0.4;
        retained->text_clicks =
            again ? (uint8_t)Min(retained->text_clicks + 1u, 3u) : 1u;
        retained->text_click_time = system->time_seconds;
        if (retained->text_clicks == 2u) {
          vkr_ui_text_select_word(buffer, retained);
        } else if (retained->text_clicks == 3u) {
          retained->text_selection = 0u;
          retained->text_cursor = buffer->length;
        } else {
          if (!vkr_ui_shift_down(system))
            retained->text_selection = retained->text_cursor;
          retained->text_dragging = true_v;
        }
      }
    }
    if (retained->text_dragging) {
      int32_t dx, dy;
      input_get_mouse_delta(system->input, &dx, &dy);
      // An unchanged release must not move the caret merely because text or
      // layout changed while the mouse was held.
      if (system->mouse_pressed ||
          ((dx || dy) && (system->mouse_released ||
                          input_is_button_down(system->input, BUTTON_LEFT)))) {
        const Vec2 endpoint = {
            (float32_t)system->mouse_x - box.x + retained->scroll_offset.x,
            (float32_t)system->mouse_y - box.y + retained->scroll_offset.y};
        retained->text_cursor =
            vkr_ui_text_mouse_cursor(&retained->text, endpoint);
      }
    }
    /* A right click focuses the field and asks its owner for the context
       menu; outside the selection it first moves the caret there. */
    if (node->hovered && system->mouse_input_layer == node->input_layer &&
        input_button_just_pressed(system->input, BUTTON_RIGHT)) {
      system->focused_id = id;
      const Vec2 point = {
          (float32_t)system->mouse_x - box.x + retained->scroll_offset.x,
          (float32_t)system->mouse_y - box.y + retained->scroll_offset.y};
      const uint32_t at = vkr_ui_text_mouse_cursor(&retained->text, point);
      if (at < Min(retained->text_cursor, retained->text_selection) ||
          at > Max(retained->text_cursor, retained->text_selection)) {
        retained->text_cursor = at;
        retained->text_selection = at;
      }
      system->text_context_id = id;
      system->text_context_px =
          (Vec2){(float32_t)system->mouse_x, (float32_t)system->mouse_y};
    }
  }
  if (system->mouse_released || system->mouse_captured || node->disabled)
    retained->text_dragging = false_v;
  retained->text_cursor = Min(retained->text_cursor, buffer->length);
  retained->text_selection = Min(retained->text_selection, buffer->length);
  while (retained->text_cursor > 0u && retained->text_cursor < buffer->length &&
         (buffer->data[retained->text_cursor] & 0xc0u) == 0x80u)
    --retained->text_cursor;
  while (retained->text_selection > 0u &&
         retained->text_selection < buffer->length &&
         (buffer->data[retained->text_selection] & 0xc0u) == 0x80u)
    --retained->text_selection;
  bool8_t changed = false_v;
  if (system->focused_id == id && system->text_undo_owner != id) {
    /* Each field starts its own history when it takes focus. */
    system->text_undo_owner = id;
    system->text_undo_count = 0u;
    system->text_redo_count = 0u;
    system->text_edit_kind = VKR_UI_TEXT_EDIT_OTHER;
  }
  if ((vkr_ui_keyboard_eligible(system, node) || command) &&
      system->focused_id == id) {
    system->focused_is_text = true_v;
    bool8_t moved = false_v;
    changed = vkr_ui_text_field_keys(system, node, buffer, config->read_only,
                                     config->multiline, command, &moved);
    // Typing or keyboard navigation takes ownership of the caret. A later
    // mouse-up must not reselect text at the old pointer position.
    if (changed || moved)
      retained->text_dragging = false_v;
  }
  if (system->text_context_id == id)
    system->text_context_state = (VkrUiTextFieldState){
        .has_text = buffer->length > 0u,
        .has_selection = retained->text_cursor != retained->text_selection,
        .read_only = config->read_only,
        .can_undo = system->text_undo_owner == id && system->text_undo_count,
        .can_redo = system->text_undo_owner == id && system->text_redo_count,
    };
  const String8 content = {.str = buffer->data, .length = buffer->length};
  if (changed && !vkr_ui_text_prepare(system, node, content, &config->text))
    return false_v;
  node->intrinsic_size =
      vkr_ui_style_clamp_size(node->intrinsic_size, &node->style);
  const VkrUiRect box =
      vkr_ui_style_content_rect(retained->last_rect, &node->style);
  const VkrUiText *text = &retained->text;
  const float32_t line_height = vkr_ui_text_line_height(text);
  if (system->focused_id == id && box.width > 0.0f && box.height > 0.0f &&
      (changed || retained->text_cursor != previous_cursor ||
       retained->text_dragging)) {
    const Vec2 caret = vkr_ui_text_cursor_position(text, retained->text_cursor);
    retained->scroll_offset.x = vkr_clamp_f32(
        retained->scroll_offset.x,
        Max(0.0f, caret.x + 2.0f * system->content_scale - box.width), caret.x);
    retained->scroll_offset.y =
        vkr_clamp_f32(retained->scroll_offset.y,
                      Max(0.0f, caret.y + line_height - box.height), caret.y);
  }
  if (node->hovered && system->mouse_wheel && text->layout.line_count > 1u)
    retained->scroll_offset.y = vkr_clamp_f32(
        retained->scroll_offset.y - system->mouse_wheel * line_height * 3.0f,
        0.0f, Max(0.0f, text->bounds.size.y - box.height));
  retained->scroll_offset.x =
      Min(retained->scroll_offset.x,
          Max(0.0f,
              text->bounds.size.x + 2.0f * system->content_scale - box.width));
  retained->scroll_offset.y = Min(retained->scroll_offset.y,
                                  Max(0.0f, text->bounds.size.y - box.height));
  return changed;
}

void vkr_ui_text_field_command(VkrUiSystem *system, VkrUiId id,
                               VkrUiTextCommand command) {
  if (system && id != VKR_UI_ID_NONE) {
    system->text_command_id = id;
    system->text_command = command;
  }
}

bool8_t vkr_ui_text_context_take(VkrUiSystem *system, VkrUiId *out_id,
                                 Vec2 *out_px, VkrUiTextFieldState *out_state) {
  if (!system || system->text_context_id == VKR_UI_ID_NONE)
    return false_v;
  *out_id = system->text_context_id;
  *out_px = system->text_context_px;
  *out_state = system->text_context_state;
  system->text_context_id = VKR_UI_ID_NONE;
  return true_v;
}

vkr_internal VkrUiTrack vkr_ui_track_resolve_points(VkrUiTrack track,
                                                    float32_t content_scale) {
  if (track.unit == VKR_UI_TRACK_PX)
    track.value *= content_scale;
  track.min_px *= content_scale;
  track.max_px *= content_scale;
  return track;
}

/* The caret blinks at about 1 Hz while its field keeps focus. */
vkr_internal bool8_t vkr_ui_caret_visible(const VkrUiSystem *system) {
  return system->reduce_motion ||
         vkr_fmod_f64(system->time_seconds, 1.06) < 0.62;
}

vkr_internal uint64_t vkr_ui_node_hash(VkrUiSystem *system,
                                       uint32_t node_index) {
  VkrUiFrameNode *node = &system->frame_nodes[node_index];
  uint64_t hash = vkr_ui_hash_bytes(UINT64_C(14695981039346656037), &node->kind,
                                    sizeof(node->kind));
  hash = vkr_ui_hash_bytes(hash, &node->id, sizeof(node->id));
  hash = vkr_ui_hash_bytes(hash, &node->placement, sizeof(node->placement));
  hash = vkr_ui_hash_bytes(hash, &node->style, sizeof(node->style));
  hash = vkr_ui_hash_bytes(hash, &node->image, sizeof(node->image));
  hash = vkr_ui_hash_bytes(hash, &node->image_size, sizeof(node->image_size));
  if (node->kind == VKR_UI_NODE_CODE)
    hash = vkr_ui_hash_bytes(hash, &node->code_hash, sizeof(node->code_hash));
  if (node->kind == VKR_UI_NODE_PLOT && node->plot) {
    const VkrUiPlotData *plot = node->plot;
    const uint32_t marks[4] = {plot->cursor, plot->range_first,
                               plot->range_last, plot->hovered};
    const Vec4 mark_colors[3] = {plot->cursor_color, plot->range_color,
                                 plot->hover_color};
    hash = vkr_ui_hash_bytes(hash, &plot->hash, sizeof(plot->hash));
    hash = vkr_ui_hash_bytes(hash, marks, sizeof(marks));
    hash = vkr_ui_hash_bytes(hash, mark_colors, sizeof(mark_colors));
  }
  if (node->kind == VKR_UI_NODE_PATH && node->path) {
    hash = vkr_ui_hash_bytes(hash, &node->path->hash, sizeof(node->path->hash));
  }
  if (node->kind == VKR_UI_NODE_BEZIER) {
    hash = vkr_ui_hash_bytes(hash, node->bezier_points,
                             sizeof(node->bezier_points));
    hash = vkr_ui_hash_bytes(hash, &node->bezier_width,
                             sizeof(node->bezier_width));
  }
  hash = vkr_ui_hash_bytes(hash, &node->intrinsic_size,
                           sizeof(node->intrinsic_size));
  hash = vkr_ui_hash_bytes(hash, &system->content_scale,
                           sizeof(system->content_scale));
  hash =
      vkr_ui_hash_bytes(hash, &node->column_count, sizeof(node->column_count));
  hash = vkr_ui_hash_bytes(hash, &node->row_count, sizeof(node->row_count));
  if (node->columns && node->column_count)
    hash = vkr_ui_hash_bytes(hash, node->columns,
                             (uint64_t)node->column_count *
                                 sizeof(*node->columns));
  if (node->rows && node->row_count)
    hash = vkr_ui_hash_bytes(hash, node->rows,
                             (uint64_t)node->row_count * sizeof(*node->rows));
  if (node->content.str && node->content.length)
    hash = vkr_ui_hash_bytes(hash, node->content.str, node->content.length);
  if (node->retained->text_live) {
    const VkrUiText *text = &node->retained->text;
    hash = vkr_ui_hash_bytes(hash, &text->geometry.revision,
                             sizeof(text->geometry.revision));
    if (text->resolved_font) {
      hash = vkr_ui_hash_bytes(hash, &text->resolved_font->id,
                               sizeof(text->resolved_font->id));
      hash = vkr_ui_hash_bytes(hash, &text->resolved_font->generation,
                               sizeof(text->resolved_font->generation));
      hash = vkr_ui_hash_bytes(hash, &text->resolved_font->type,
                               sizeof(text->resolved_font->type));
      hash = vkr_ui_hash_bytes(hash, &text->resolved_font->atlas,
                               sizeof(text->resolved_font->atlas));
      hash = vkr_ui_hash_bytes(hash, &text->resolved_font->sdf_distance_range,
                               sizeof(text->resolved_font->sdf_distance_range));
      hash = vkr_ui_hash_bytes(hash, &text->resolved_font->mtsdf_unit_range,
                               sizeof(text->resolved_font->mtsdf_unit_range));
      hash = vkr_ui_hash_bytes(hash, &text->resolved_font->em_size,
                               sizeof(text->resolved_font->em_size));
    }
  }
  hash = vkr_ui_hash_bytes(hash, &node->retained->scroll_offset,
                           sizeof(node->retained->scroll_offset));
  hash = vkr_ui_hash_bytes(hash, &node->retained->text_cursor,
                           sizeof(node->retained->text_cursor));
  hash = vkr_ui_hash_bytes(hash, &node->retained->text_selection,
                           sizeof(node->retained->text_selection));
  hash = vkr_ui_hash_bytes(hash, &node->slider_fraction,
                           sizeof(node->slider_fraction));
  hash = vkr_ui_hash_bytes(hash, &node->checked, sizeof(node->checked));
  hash = vkr_ui_hash_bytes(hash, &node->icon, sizeof(node->icon));
  hash =
      vkr_ui_hash_bytes(hash, &node->icon_size_px, sizeof(node->icon_size_px));
  hash = vkr_ui_hash_bytes(hash, &node->icon_color, sizeof(node->icon_color));
  hash = vkr_ui_hash_bytes(hash, &node->trailing_icon,
                           sizeof(node->trailing_icon));
  hash = vkr_ui_hash_bytes(hash, &node->trailing_icon_size_px,
                           sizeof(node->trailing_icon_size_px));
  hash = vkr_ui_hash_bytes(hash, &node->disabled, sizeof(node->disabled));
  hash = vkr_ui_hash_bytes(hash, &node->fill, sizeof(node->fill));
  hash = vkr_ui_hash_bytes(hash, &node->center, sizeof(node->center));
  hash = vkr_ui_hash_bytes(hash, &node->leading, sizeof(node->leading));
  hash = vkr_ui_hash_bytes(hash, &node->hovered, sizeof(node->hovered));
  hash = vkr_ui_hash_bytes(hash, &node->retained->hover_t,
                           sizeof(node->retained->hover_t));
  hash = vkr_ui_hash_bytes(hash, &node->retained->active_t,
                           sizeof(node->retained->active_t));
  hash = vkr_ui_hash_bytes(hash, &node->retained->focus_t,
                           sizeof(node->retained->focus_t));
  if (node->kind == VKR_UI_NODE_TEXT_FIELD && system->focused_id == node->id) {
    const bool8_t caret_on = vkr_ui_caret_visible(system);
    hash = vkr_ui_hash_bytes(hash, &caret_on, sizeof(caret_on));
  }
  hash = vkr_ui_hash_bytes(hash, &node->active, sizeof(node->active));
  hash = vkr_ui_hash_bytes(hash, &node->clip_children,
                           sizeof(node->clip_children));
  const bool8_t focused = system->focused_id == node->id;
  hash = vkr_ui_hash_bytes(hash, &focused, sizeof(focused));
  for (uint32_t child = node->first_child; child != VKR_UI_NODE_NONE;
       child = system->frame_nodes[child].next_sibling) {
    const uint64_t child_hash = vkr_ui_node_hash(system, child);
    hash = vkr_ui_hash_bytes(hash, &child_hash, sizeof(child_hash));
  }
  node->build_hash = hash;
  return hash;
}

vkr_internal bool8_t vkr_ui_node_is_container(const VkrUiFrameNode *node) {
  return node->kind == VKR_UI_NODE_ROOT || node->kind == VKR_UI_NODE_PANEL ||
         node->kind == VKR_UI_NODE_SCROLL;
}

vkr_internal VkrUiGridItem vkr_ui_grid_item_from_node(VkrUiSystem *system,
                                                      VkrUiFrameNode *child,
                                                      uint32_t columns,
                                                      uint32_t rows) {
  const VkrUiPlacement placement = child->placement;
  const VkrUiEdges placement_margin = {
      .top = placement.margin_pt.top * system->content_scale,
      .right = placement.margin_pt.right * system->content_scale,
      .bottom = placement.margin_pt.bottom * system->content_scale,
      .left = placement.margin_pt.left * system->content_scale,
  };
  const bool8_t text_widget =
      !child->fill &&
      (child->kind == VKR_UI_NODE_LABEL || child->kind == VKR_UI_NODE_BUTTON ||
       child->kind == VKR_UI_NODE_CHECKBOX ||
       child->kind == VKR_UI_NODE_TEXT_FIELD);
  /* Like a native input, a field keeps its cell's width instead of growing
     with what is typed; its height stays its content's. */
  const bool8_t content_width =
      text_widget && child->kind != VKR_UI_NODE_TEXT_FIELD;
  return (VkrUiGridItem){
      .column = placement.column,
      .row = placement.row,
      .column_span = Min(placement.column_span, columns),
      .row_span = Min(placement.row_span, rows),
      .justify = content_width && placement.justify == VKR_UI_ALIGN_STRETCH
                     ? VKR_UI_ALIGN_START
                     : placement.justify,
      .align = text_widget && placement.align == VKR_UI_ALIGN_STRETCH
                   ? VKR_UI_ALIGN_START
                   : placement.align,
      .intrinsic_size_px = child->intrinsic_size,
      .max_size_px = child->style.max_size_px,
      .margin_px = vkr_ui_edges_add(placement_margin, child->style.margin_px),
  };
}

static bool8_t vkr_ui_grid_failure(VkrUiSystem *system,
                                   const VkrUiFrameNode *node,
                                   const char *stage) {
  if (!system->layout_failure_warning_emitted) {
    log_error("UI %s failed: node=%llu kind=%u grid=%ux%u size=%.1fx%.1f",
              stage, (unsigned long long)node->id, (uint32_t)node->kind,
              node->column_count, node->row_count, node->rect.width,
              node->rect.height);
    for (uint32_t child = node->first_child; child != VKR_UI_NODE_NONE;
         child = system->frame_nodes[child].next_sibling) {
      const VkrUiFrameNode *item = &system->frame_nodes[child];
      log_error("UI child=%llu kind=%u cell=%u,%u span=%u,%u "
                "intrinsic=%.1fx%.1f margin=%.1f,%.1f,%.1f,%.1f text='%.*s'",
                (unsigned long long)item->id, (uint32_t)item->kind,
                item->placement.column, item->placement.row,
                item->placement.column_span, item->placement.row_span,
                item->intrinsic_size.x, item->intrinsic_size.y,
                item->placement.margin_pt.top, item->placement.margin_pt.right,
                item->placement.margin_pt.bottom,
                item->placement.margin_pt.left,
                (int)Min(item->content.length, 80u),
                item->content.str ? item->content.str : (uint8_t *)"");
    }
    system->layout_failure_warning_emitted = true_v;
  }
  return false_v;
}

vkr_internal bool8_t vkr_ui_container_intrinsic(VkrUiSystem *system,
                                                VkrUiFrameNode *node,
                                                Vec2 *out_size) {
  uint32_t child_count = 0u;
  for (uint32_t child_index = node->first_child;
       child_index != VKR_UI_NODE_NONE;
       child_index = system->frame_nodes[child_index].next_sibling) {
    VkrUiFrameNode *child = &system->frame_nodes[child_index];
    if (vkr_ui_node_is_container(child) &&
        !vkr_ui_container_intrinsic(system, child, &child->intrinsic_size))
      return false_v;
    child_count++;
  }

  Vec2 content = {0};
  if (child_count > 0u) {
    const uint32_t columns = node->column_count ? node->column_count : 1u;
    const uint32_t rows = node->row_count ? node->row_count : 1u;
    VkrUiTrack *column_tracks = vkr_allocator_alloc(
        system->frame_allocator, (uint64_t)columns * sizeof(*column_tracks),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    VkrUiTrack *row_tracks = vkr_allocator_alloc(
        system->frame_allocator, (uint64_t)rows * sizeof(*row_tracks),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    float32_t *column_sizes = vkr_allocator_alloc(
        system->frame_allocator, (uint64_t)columns * sizeof(*column_sizes),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    float32_t *row_sizes = vkr_allocator_alloc(
        system->frame_allocator, (uint64_t)rows * sizeof(*row_sizes),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    VkrUiGridItem *items = vkr_allocator_alloc(
        system->frame_allocator, (uint64_t)child_count * sizeof(*items),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    VkrUiGridCell *cells = vkr_allocator_alloc(
        system->frame_allocator, (uint64_t)child_count * sizeof(*cells),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    const uint64_t cell_count = (uint64_t)columns * rows;
    uint8_t *occupancy = vkr_allocator_alloc(
        system->frame_allocator, cell_count, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!column_tracks || !row_tracks || !column_sizes || !row_sizes ||
        !items || !cells || !occupancy)
      return vkr_ui_grid_failure(system, node, "intrinsic allocation");

    for (uint32_t i = 0u; i < columns; ++i)
      column_tracks[i] = vkr_ui_track_resolve_points(
          node->columns ? node->columns[i] : VKR_UI_ONE_FR_TRACK,
          system->content_scale);
    for (uint32_t i = 0u; i < rows; ++i)
      row_tracks[i] = vkr_ui_track_resolve_points(
          node->rows ? node->rows[i] : VKR_UI_ONE_FR_TRACK,
          system->content_scale);

    uint32_t child_cursor = 0u;
    for (uint32_t child_index = node->first_child;
         child_index != VKR_UI_NODE_NONE;
         child_index = system->frame_nodes[child_index].next_sibling) {
      items[child_cursor++] = vkr_ui_grid_item_from_node(
          system, &system->frame_nodes[child_index], columns, rows);
    }
    if (!vkr_ui_grid_resolve_placements(columns, rows, items, child_count,
                                        occupancy, (uint32_t)cell_count, cells,
                                        child_count))
      return vkr_ui_grid_failure(system, node, "intrinsic placement");
    VkrUiGridIntrinsicOutput intrinsic = {0};
    if (!vkr_ui_grid_measure_intrinsic(column_tracks, columns, row_tracks, rows,
                                       node->style.gap_px, items, cells,
                                       child_count, column_sizes, columns,
                                       row_sizes, rows, &intrinsic))
      return false_v;
    content = (Vec2){intrinsic.width_px, intrinsic.height_px};
  }

  content.x += node->style.padding_px.left + node->style.padding_px.right +
               node->style.border_px.left + node->style.border_px.right;
  content.y += node->style.padding_px.top + node->style.padding_px.bottom +
               node->style.border_px.top + node->style.border_px.bottom;
  *out_size = vkr_ui_style_clamp_size(content, &node->style);
  return true_v;
}

vkr_internal bool8_t vkr_ui_reuse_layout(VkrUiSystem *system,
                                         uint32_t node_index) {
  VkrUiFrameNode *node = &system->frame_nodes[node_index];
  if (!vkr_ui_rect_has_area(node->retained->last_rect))
    return false_v;
  node->rect = node->retained->last_rect;
  node->clip = node->retained->last_clip;
  for (uint32_t child = node->first_child; child != VKR_UI_NODE_NONE;
       child = system->frame_nodes[child].next_sibling) {
    if (!vkr_ui_reuse_layout(system, child))
      return false_v;
  }
  return true_v;
}

vkr_internal bool8_t vkr_ui_layout_node(VkrUiSystem *system,
                                        uint32_t node_index, VkrUiRect rect,
                                        VkrUiRect parent_clip) {
  VkrUiFrameNode *node = &system->frame_nodes[node_index];
  if (node->retained->build_hash == node->build_hash &&
      vkr_ui_rect_equal(node->retained->last_rect, rect) &&
      vkr_ui_rect_equal(node->retained->last_clip, parent_clip) &&
      vkr_ui_reuse_layout(system, node_index))
    return true_v;

  node->rect = rect;
  node->clip = parent_clip;
  node->retained->last_rect = rect;
  node->retained->last_clip = parent_clip;
  node->retained->build_hash = node->build_hash;
  if (node->first_child == VKR_UI_NODE_NONE)
    return true_v;

  const VkrUiRect content_rect = vkr_ui_style_content_rect(rect, &node->style);
  VkrUiRect child_clip = node->clip_children
                             ? vkr_ui_rect_intersect(parent_clip, content_rect)
                             : parent_clip;
  if (node->kind == VKR_UI_NODE_SCROLL && system->focused_id == node->id) {
    // Descendants must not paint over the container's keyboard focus ring.
    const float32_t inset = 2.0f * Max(1.0f, system->content_scale);
    const VkrUiRect focus_content =
        vkr_ui_rect_inset(rect, (VkrUiEdges){inset, inset, inset, inset});
    child_clip = vkr_ui_rect_intersect(child_clip, focus_content);
  }
  const uint32_t columns = node->column_count ? node->column_count : 1u;
  const uint32_t rows = node->row_count ? node->row_count : 1u;
  uint32_t child_count = 0u;
  for (uint32_t child = node->first_child; child != VKR_UI_NODE_NONE;
       child = system->frame_nodes[child].next_sibling)
    child_count++;

  VkrUiTrack *column_tracks = vkr_allocator_alloc(
      system->frame_allocator, (uint64_t)columns * sizeof(*column_tracks),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrUiTrack *row_tracks = vkr_allocator_alloc(
      system->frame_allocator, (uint64_t)rows * sizeof(*row_tracks),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  float32_t *column_offsets = vkr_allocator_alloc(
      system->frame_allocator, (uint64_t)columns * sizeof(*column_offsets),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  float32_t *column_sizes = vkr_allocator_alloc(
      system->frame_allocator, (uint64_t)columns * sizeof(*column_sizes),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  float32_t *column_auto = vkr_allocator_alloc(
      system->frame_allocator, (uint64_t)columns * sizeof(*column_auto),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  float32_t *row_offsets = vkr_allocator_alloc(
      system->frame_allocator, (uint64_t)rows * sizeof(*row_offsets),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  float32_t *row_sizes = vkr_allocator_alloc(
      system->frame_allocator, (uint64_t)rows * sizeof(*row_sizes),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  float32_t *row_auto = vkr_allocator_alloc(system->frame_allocator,
                                            (uint64_t)rows * sizeof(*row_auto),
                                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrUiGridItem *items = vkr_allocator_alloc(
      system->frame_allocator, (uint64_t)child_count * sizeof(*items),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrUiRect *rects = vkr_allocator_alloc(system->frame_allocator,
                                         (uint64_t)child_count * sizeof(*rects),
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrUiGridCell *cells = vkr_allocator_alloc(
      system->frame_allocator, (uint64_t)child_count * sizeof(*cells),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  const uint64_t cell_count = (uint64_t)columns * rows;
  uint8_t *occupancy = vkr_allocator_alloc(system->frame_allocator, cell_count,
                                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!column_tracks || !row_tracks || !column_offsets || !column_sizes ||
      !column_auto || !row_offsets || !row_sizes || !row_auto || !items ||
      !rects || !cells || !occupancy)
    return false_v;
  MemZero(column_auto, (uint64_t)columns * sizeof(*column_auto));
  MemZero(row_auto, (uint64_t)rows * sizeof(*row_auto));
  for (uint32_t i = 0u; i < columns; ++i)
    column_tracks[i] = vkr_ui_track_resolve_points(
        node->columns ? node->columns[i] : VKR_UI_ONE_FR_TRACK,
        system->content_scale);
  for (uint32_t i = 0u; i < rows; ++i)
    row_tracks[i] = vkr_ui_track_resolve_points(
        node->rows ? node->rows[i] : VKR_UI_ONE_FR_TRACK,
        system->content_scale);

  uint32_t child_cursor = 0u;
  for (uint32_t child_index = node->first_child;
       child_index != VKR_UI_NODE_NONE;
       child_index = system->frame_nodes[child_index].next_sibling) {
    VkrUiFrameNode *child = &system->frame_nodes[child_index];
    items[child_cursor++] =
        vkr_ui_grid_item_from_node(system, child, columns, rows);
  }

  if (!vkr_ui_grid_resolve_placements(columns, rows, items, child_count,
                                      occupancy, (uint32_t)cell_count, cells,
                                      child_count))
    return vkr_ui_grid_failure(system, node, "layout placement");
  for (uint32_t i = 0u; i < child_count; ++i) {
    const VkrUiGridItem *item = &items[i];
    const VkrUiGridCell cell = cells[i];
    if (item->column_span == 1u)
      column_auto[cell.column] =
          Max(column_auto[cell.column], item->intrinsic_size_px.x +
                                            item->margin_px.left +
                                            item->margin_px.right);
    if (item->row_span == 1u)
      row_auto[cell.row] = Max(row_auto[cell.row], item->intrinsic_size_px.y +
                                                       item->margin_px.top +
                                                       item->margin_px.bottom);
  }

  VkrUiGridAxisOutput column_output = {.offsets_px = column_offsets,
                                       .sizes_px = column_sizes,
                                       .capacity = columns};
  VkrUiGridAxisOutput row_output = {
      .offsets_px = row_offsets, .sizes_px = row_sizes, .capacity = rows};
  if (!vkr_ui_grid_resolve_tracks(column_tracks, columns, content_rect.width,
                                  node->style.gap_px, column_auto,
                                  &column_output) ||
      !vkr_ui_grid_resolve_tracks(row_tracks, rows, content_rect.height,
                                  node->style.gap_px, row_auto, &row_output))
    return vkr_ui_grid_failure(system, node, "track resolution");
  if (node->kind == VKR_UI_NODE_SCROLL) {
    const float32_t max_scroll =
        Max(0.0f, row_output.resolved_extent_px - content_rect.height);
    node->retained->scroll_offset.y =
        Min(node->retained->scroll_offset.y, max_scroll);
    node->retained->content_extent = row_output.resolved_extent_px;
  }
  if (!vkr_ui_grid_arrange_items(
          content_rect,
          (VkrUiGridAxisView){column_offsets, column_sizes, columns},
          (VkrUiGridAxisView){row_offsets, row_sizes, rows}, items, child_count,
          occupancy, (uint32_t)cell_count, rects, child_count))
    return vkr_ui_grid_failure(system, node, "item arrangement");

  child_cursor = 0u;
  for (uint32_t child_index = node->first_child;
       child_index != VKR_UI_NODE_NONE;
       child_index = system->frame_nodes[child_index].next_sibling) {
    VkrUiRect child_rect = rects[child_cursor++];
    if (node->kind == VKR_UI_NODE_SCROLL)
      child_rect.y -= node->retained->scroll_offset.y;
    if (!vkr_ui_layout_node(system, child_index, child_rect, child_clip))
      return false_v;
  }
  return true_v;
}

vkr_internal Vec4 vkr_ui_linear_color(Vec4 color) {
  return vkr_srgb_color_to_linear(color);
}

vkr_internal bool8_t vkr_ui_edges_have_extent(VkrUiEdges edges) {
  return edges.top > 0.0f || edges.right > 0.0f || edges.bottom > 0.0f ||
         edges.left > 0.0f;
}

vkr_internal Vec4 vkr_ui_inner_radii(Vec4 radii, VkrUiEdges border) {
  return (Vec4){
      Max(0.0f, radii.x - Max(border.top, border.left)),
      Max(0.0f, radii.y - Max(border.top, border.right)),
      Max(0.0f, radii.z - Max(border.bottom, border.right)),
      Max(0.0f, radii.w - Max(border.bottom, border.left)),
  };
}

vkr_internal void vkr_ui_emit_rect(VkrUiDrawBuffer *buffer, VkrUiRect rect,
                                   Vec4 color, Vec4 radii) {
  if (!vkr_ui_rect_has_area(rect) || !vkr_ui_color_visible(color))
    return;
  const bool8_t rounded =
      radii.x > 0.0f || radii.y > 0.0f || radii.z > 0.0f || radii.w > 0.0f;
  if (rounded)
    (void)vkr_ui_draw_buffer_box(buffer, rect, vkr_ui_linear_color(color),
                                 radii, (Vec4){0}, 0.0f, 0.0f);
  else
    (void)vkr_ui_draw_buffer_solid(buffer, rect, vkr_ui_linear_color(color));
}

/* One box with an inner border. A transparent fill still draws the border. */
vkr_internal void vkr_ui_emit_box(VkrUiDrawBuffer *buffer, VkrUiRect rect,
                                  Vec4 fill, Vec4 radii, Vec4 border_color,
                                  float32_t border_px) {
  if (!vkr_ui_rect_has_area(rect))
    return;
  const bool8_t border = border_px > 0.0f && vkr_ui_color_visible(border_color);
  if (!border && !vkr_ui_color_visible(fill))
    return;
  if (!border) {
    vkr_ui_emit_rect(buffer, rect, fill, radii);
    return;
  }
  /* A transparent fill keeps the border hue so the edge blend never darkens
   * toward black. */
  Vec4 linear_fill = vkr_ui_linear_color(fill);
  const Vec4 linear_border = vkr_ui_linear_color(border_color);
  if (!vkr_ui_color_visible(fill))
    linear_fill =
        (Vec4){linear_border.x, linear_border.y, linear_border.z, 0.0f};
  (void)vkr_ui_draw_buffer_box(buffer, rect, linear_fill, radii, linear_border,
                               border_px, 0.0f);
}

/* Feathered box beneath a surface. */
vkr_internal void vkr_ui_emit_shadow(VkrUiDrawBuffer *buffer, VkrUiRect rect,
                                     Vec4 radii, Vec4 color, Vec2 offset_px,
                                     float32_t blur_px) {
  if (!vkr_ui_rect_has_area(rect) || !vkr_ui_color_visible(color) ||
      blur_px <= 0.0f)
    return;
  rect.x += offset_px.x;
  rect.y += offset_px.y;
  const float32_t spread = blur_px * 0.5f;
  const float32_t limit = Min(rect.width, rect.height) * 0.5f;
  const Vec4 shadow_radii = {
      Min(radii.x + spread, limit), Min(radii.y + spread, limit),
      Min(radii.z + spread, limit), Min(radii.w + spread, limit)};
  (void)vkr_ui_draw_buffer_box(buffer, rect, vkr_ui_linear_color(color),
                               shadow_radii, (Vec4){0}, 0.0f, blur_px);
}

typedef struct VkrUiIconGlyph {
  uint32_t codepoint;
  bool8_t fill;
} VkrUiIconGlyph;

/* Phosphor codepoints; the cooked charsets list exactly these glyphs. */
vkr_global const VkrUiIconGlyph vkr_ui_icon_glyphs[VKR_UI_ICON_COUNT] = {
    [VKR_UI_ICON_PLAY] = {0xE3D0, true_v},              /* play */
    [VKR_UI_ICON_PAUSE] = {0xE39E, true_v},             /* pause */
    [VKR_UI_ICON_MONITOR_PLAY] = {0xE58C, false_v},     /* monitor-play */
    [VKR_UI_ICON_MONITOR_STOP] = {0xE32E, false_v},     /* monitor */
    [VKR_UI_ICON_HIERARCHY] = {0xE67C, false_v},        /* tree-structure */
    [VKR_UI_ICON_INSPECTOR] = {0xE434, false_v},        /* sliders-horizontal */
    [VKR_UI_ICON_CONSOLE] = {0xEAE8, false_v},          /* terminal-window */
    [VKR_UI_ICON_SCENE] = {0xE1DA, false_v},            /* cube */
    [VKR_UI_ICON_BAKERY] = {0xE764, false_v},           /* cooking-pot */
    [VKR_UI_ICON_SCENE_LOAD] = {0xE20C, false_v},       /* download-simple */
    [VKR_UI_ICON_SCENE_UNLOAD] = {0xE4C0, false_v},     /* upload-simple */
    [VKR_UI_ICON_CAMERA] = {0xE4DA, false_v},           /* video-camera */
    [VKR_UI_ICON_GRIP] = {0xEAE2, false_v},             /* dots-six-vertical */
    [VKR_UI_ICON_LOG_FATAL] = {0xE916, true_v},         /* skull */
    [VKR_UI_ICON_LOG_ERROR] = {0xE4F8, true_v},         /* x-circle */
    [VKR_UI_ICON_LOG_WARNING] = {0xE4E0, true_v},       /* warning */
    [VKR_UI_ICON_LOG_INFO] = {0xE2CE, true_v},          /* info */
    [VKR_UI_ICON_LOG_DEBUG] = {0xE5F4, false_v},        /* bug */
    [VKR_UI_ICON_LOG_TRACE] = {0xE39C, false_v},        /* path */
    [VKR_UI_ICON_PROJECT] = {0xE24A, false_v},          /* folder-notch */
    [VKR_UI_ICON_FOLDER] = {0xE24A, true_v},            /* folder */
    [VKR_UI_ICON_CONTENT] = {0xE464, false_v},          /* squares-four */
    [VKR_UI_ICON_TEXTURE] = {0xE2CA, false_v},          /* image */
    [VKR_UI_ICON_MATERIAL] = {0xEE66, false_v},         /* sphere */
    [VKR_UI_ICON_MESH] = {0xE6D0, false_v},             /* polygon */
    [VKR_UI_ICON_FONT] = {0xE6EE, false_v},             /* text-aa */
    [VKR_UI_ICON_LIGHT] = {0xE2DC, false_v},            /* lightbulb */
    [VKR_UI_ICON_ENVIRONMENT] = {0xE7AE, false_v},      /* mountains */
    [VKR_UI_ICON_PROBE] = {0xE28E, false_v},            /* globe-simple */
    [VKR_UI_ICON_REFRESH] = {0xE094, false_v},          /* arrows-clockwise */
    [VKR_UI_ICON_ADD] = {0xE3D4, false_v},              /* plus */
    [VKR_UI_ICON_SEARCH] = {0xE30C, false_v},           /* magnifying-glass */
    [VKR_UI_ICON_STOP] = {0xE46C, true_v},              /* stop */
    [VKR_UI_ICON_STEP] = {0xE5A6, true_v},              /* skip-forward */
    [VKR_UI_ICON_CHEVRON_DOWN] = {0xE136, false_v},     /* caret-down */
    [VKR_UI_ICON_CHEVRON_RIGHT] = {0xE13A, false_v},    /* caret-right */
    [VKR_UI_ICON_CHEVRON_LEFT] = {0xE138, false_v},     /* caret-left */
    [VKR_UI_ICON_CHEVRON_UP] = {0xE13C, false_v},       /* caret-up */
    [VKR_UI_ICON_DISCLOSURE_OPEN] = {0xE136, true_v},   /* caret-down */
    [VKR_UI_ICON_DISCLOSURE_CLOSED] = {0xE13A, true_v}, /* caret-right */
    [VKR_UI_ICON_CLOSE] = {0xE4F6, false_v},            /* x */
    [VKR_UI_ICON_CHECK] = {0xE182, false_v},            /* check */
    [VKR_UI_ICON_MINUS] = {0xE32A, false_v},            /* minus */
    [VKR_UI_ICON_MORE] = {0xE1FE, false_v},             /* dots-three */
    [VKR_UI_ICON_MENU] = {0xE2F0, false_v},             /* list */
    [VKR_UI_ICON_ARROW_LEFT] = {0xE058, false_v},       /* arrow-left */
    [VKR_UI_ICON_ARROW_RIGHT] = {0xE06C, false_v},      /* arrow-right */
    [VKR_UI_ICON_ARROW_UP] = {0xE08E, false_v},         /* arrow-up */
    [VKR_UI_ICON_SORT_ASCENDING] = {0xE444, false_v},   /* sort-ascending */
    [VKR_UI_ICON_SORT_DESCENDING] = {0xE446, false_v},  /* sort-descending */
    [VKR_UI_ICON_EYE] = {0xE220, false_v},              /* eye */
    [VKR_UI_ICON_EYE_SLASH] = {0xE224, false_v},        /* eye-slash */
    [VKR_UI_ICON_LOCK] = {0xE2FA, false_v},             /* lock */
    [VKR_UI_ICON_UNLOCK] = {0xE306, false_v},           /* lock-open */
    [VKR_UI_ICON_TRASH] = {0xE4A6, false_v},            /* trash */
    [VKR_UI_ICON_COPY] = {0xE1CA, false_v},             /* copy */
    [VKR_UI_ICON_PASTE] = {0xE196, false_v},            /* clipboard */
    [VKR_UI_ICON_DUPLICATE] = {0xE1CC, false_v},        /* copy-simple */
    [VKR_UI_ICON_RENAME] = {0xE3B4, false_v},           /* pencil-simple */
    [VKR_UI_ICON_SAVE] = {0xE248, false_v},             /* floppy-disk */
    [VKR_UI_ICON_UNDO] = {0xE038, false_v},       /* arrow-counter-clockwise */
    [VKR_UI_ICON_REDO] = {0xE036, false_v},       /* arrow-clockwise */
    [VKR_UI_ICON_RESET] = {0xE08A, false_v},      /* arrow-u-up-left */
    [VKR_UI_ICON_FRAME] = {0xE626, false_v},      /* frame-corners */
    [VKR_UI_ICON_SETTINGS] = {0xE270, false_v},   /* gear */
    [VKR_UI_ICON_GRAPHICS] = {0xE32E, false_v},   /* monitor */
    [VKR_UI_ICON_METRICS] = {0xE154, false_v},    /* chart-line */
    [VKR_UI_ICON_MEMORY] = {0xE9C4, false_v},     /* memory */
    [VKR_UI_ICON_DRAWS] = {0xE466, false_v},      /* stack */
    [VKR_UI_ICON_HELP] = {0xE3E8, false_v},       /* question */
    [VKR_UI_ICON_COMMAND] = {0xE1C4, false_v},    /* command */
    [VKR_UI_ICON_KEYBOARD] = {0xE2D8, false_v},   /* keyboard */
    [VKR_UI_ICON_ANIMATION] = {0xE792, false_v},  /* film-strip */
    [VKR_UI_ICON_PHYSICS] = {0xE5E4, false_v},    /* atom */
    [VKR_UI_ICON_COLLIDER] = {0xE6CE, false_v},   /* bounding-box */
    [VKR_UI_ICON_RIGID_BODY] = {0xED0A, false_v}, /* cube-focus */
    [VKR_UI_ICON_JOINT] = {0xE2E2, false_v},      /* link */
    [VKR_UI_ICON_BONE] = {0xE7F2, false_v},       /* bone */
    [VKR_UI_ICON_SKIN] = {0xE72E, false_v},       /* person-simple */
    [VKR_UI_ICON_CAMERA_ENTITY] = {0xE10E, false_v},     /* camera */
    [VKR_UI_ICON_DIRECTIONAL_LIGHT] = {0xE472, false_v}, /* sun */
    [VKR_UI_ICON_SPOT_LIGHT] = {0xE246, false_v},        /* flashlight */
    [VKR_UI_ICON_POINT_LIGHT] = {0xE63C, false_v}, /* lightbulb-filament */
    [VKR_UI_ICON_RECT_LIGHT] = {0xE462, false_v},  /* square-half */
    [VKR_UI_ICON_SKY] = {0xE540, false_v},         /* cloud-sun */
    [VKR_UI_ICON_FOG] = {0xE53C, false_v},         /* cloud-fog */
    [VKR_UI_ICON_VOLUME] = {0xEC7C, false_v},      /* cube-transparent */
    [VKR_UI_ICON_EMPTY] = {0xE602, false_v},       /* circle-dashed */
    [VKR_UI_ICON_MOVE] = {0xE0A4, false_v},        /* arrows-out-cardinal */
    [VKR_UI_ICON_ROTATE] = {0xE016, false_v},      /* arrow-arc-right */
    [VKR_UI_ICON_SCALE] = {0xE0A2, false_v},       /* arrows-out */
    [VKR_UI_ICON_SELECT] = {0xE1DC, false_v},      /* cursor */
    [VKR_UI_ICON_SNAP] = {0xE680, false_v},        /* magnet */
    [VKR_UI_ICON_WORLD] = {0xE288, false_v},       /* globe */
    [VKR_UI_ICON_LOCAL] = {0xED0A, false_v},       /* cube-focus */
    [VKR_UI_ICON_GRID] = {0xE296, false_v},        /* grid-four */
    [VKR_UI_ICON_PERSPECTIVE] = {0xEBE6, false_v}, /* perspective */
    [VKR_UI_ICON_VIEW_MODE] = {0xE18C, false_v},   /* circle-half */
    [VKR_UI_ICON_LAYERS] = {0xE468, false_v},      /* stack-simple */
    [VKR_UI_ICON_IMPORT] = {0xE20A, false_v},      /* download */
    [VKR_UI_ICON_EXPORT] = {0xEAF0, false_v},      /* export */
    [VKR_UI_ICON_REVEAL] = {0xE256, false_v},      /* folder-open */
    [VKR_UI_ICON_SPINNER] = {0xEB44, false_v},     /* circle-notch */
    [VKR_UI_ICON_BELL] = {0xE0CE, false_v},        /* bell */
    [VKR_UI_ICON_CPU] = {0xE610, false_v},         /* cpu */
    [VKR_UI_ICON_GPU] = {0xE612, false_v},         /* graphics-card */
    [VKR_UI_ICON_SPEED] = {0xE628, false_v},       /* gauge */
    [VKR_UI_ICON_FILTER] = {0xE266, false_v},      /* funnel */
    [VKR_UI_ICON_LIST] = {0xE2F2, false_v},        /* list-bullets */
    [VKR_UI_ICON_FILE] = {0xE230, false_v},        /* file */
    [VKR_UI_ICON_PIN] = {0xE3E2, false_v},         /* push-pin */
    [VKR_UI_ICON_MAXIMIZE] = {0xE0A6, false_v},    /* arrows-out-simple */
    [VKR_UI_ICON_MINIMIZE] = {0xE09E, false_v},    /* arrows-in-simple */
    [VKR_UI_ICON_LAYOUT] = {0xE6D6, false_v},      /* layout */
    [VKR_UI_ICON_CHECK_CIRCLE] = {0xE184, true_v}, /* check-circle */
    [VKR_UI_ICON_PLANET] = {0xE652, false_v},      /* planet */
    [VKR_UI_ICON_RECORD] = {0xE3EE, true_v},       /* record */
    [VKR_UI_ICON_PLUS_CIRCLE] = {0xE3D6, false_v}, /* plus-circle */
    [VKR_UI_ICON_SIDEBAR] = {0xEC24, false_v},     /* sidebar-simple */
    [VKR_UI_ICON_WINDOW] = {0xE5DA, false_v},      /* app-window */
    [VKR_UI_ICON_TAG] = {0xE478, false_v},         /* tag */
    [VKR_UI_ICON_HOME] = {0xE2C2, false_v},        /* house */
    [VKR_UI_ICON_DOT] = {0xE18A, true_v},          /* circle */
    [VKR_UI_ICON_ARROW_DOWN] = {0xE03E, false_v},  /* arrow-down */
    [VKR_UI_ICON_FOLLOW_TAIL] = {0xE05C, false_v}, /* arrow-line-down */
    [VKR_UI_ICON_BROOM] = {0xEC54, false_v},       /* broom */
    [VKR_UI_ICON_CROSSHAIR] = {0xE1D6, false_v},   /* crosshair */
    [VKR_UI_ICON_HAND] = {0xE298, false_v},        /* hand */
    [VKR_UI_ICON_WRENCH] = {0xE5D4, false_v},      /* wrench */
    [VKR_UI_ICON_PALETTE] = {0xE6C8, false_v},     /* palette */
    [VKR_UI_ICON_THERMOMETER] = {0xE5C6, false_v}, /* thermometer */
    [VKR_UI_ICON_ANGLE] = {0xE7BC, false_v},       /* angle */
    [VKR_UI_ICON_RULER] = {0xE6B8, false_v},       /* ruler */
    [VKR_UI_ICON_TIMER] = {0xE492, false_v},       /* timer */
    [VKR_UI_ICON_GRAPH] = {0xEB58, false_v},       /* graph */
    [VKR_UI_ICON_SPARKLE] = {0xE6A2, false_v},     /* sparkle */
    [VKR_UI_ICON_ZOOM_IN] = {0xE310, false_v},     /* magnifying-glass-plus */
    [VKR_UI_ICON_ZOOM_OUT] = {0xE30E, false_v},    /* magnifying-glass-minus */
    [VKR_UI_ICON_CARET_UP_DOWN] = {0xE140, false_v},   /* caret-up-down */
    [VKR_UI_ICON_KEBAB] = {0xE208, false_v},           /* dots-three-vertical */
    [VKR_UI_ICON_CHART_BAR] = {0xE150, false_v},       /* chart-bar */
    [VKR_UI_ICON_LIGHTNING] = {0xE2DE, false_v},       /* lightning */
    [VKR_UI_ICON_GAME_CONTROLLER] = {0xE26E, false_v}, /* game-controller */
    [VKR_UI_ICON_PERSON_WALK] = {0xE73A, false_v},     /* person-simple-walk */
    [VKR_UI_ICON_SHAPES] = {0xEC5E, false_v},          /* shapes */
    [VKR_UI_ICON_DATABASE] = {0xE1DE, false_v},        /* database */
    [VKR_UI_ICON_HARD_DRIVES] = {0xE2A0, false_v},     /* hard-drives */
    [VKR_UI_ICON_CLOCK] = {0xE19A, false_v},           /* clock */
    [VKR_UI_ICON_BRAND] = {0xE1DA, true_v},            /* cube */
    [VKR_UI_ICON_EYE_FILL] = {0xE220, true_v},         /* eye */
    [VKR_UI_ICON_LOCK_FILL] = {0xE2FA, true_v},        /* lock */
    [VKR_UI_ICON_WARNING_FILL] = {0xE4E2, true_v},     /* warning-circle */
    [VKR_UI_ICON_INFO_FILL] = {0xE2CE, true_v},        /* info */
    [VKR_UI_ICON_CHECK_SQUARE] = {0xE186, false_v},    /* check-square */
    [VKR_UI_ICON_SQUARE] = {0xE45E, false_v},          /* square */
    [VKR_UI_ICON_CIRCLE] = {0xE18A, false_v},          /* circle */
    [VKR_UI_ICON_SUN_DIM] = {0xE474, false_v},         /* sun-dim */
    [VKR_UI_ICON_MOON] = {0xE330, false_v},            /* moon */
    [VKR_UI_ICON_CLOUD] = {0xE1AA, false_v},           /* cloud */
    [VKR_UI_ICON_DROP] = {0xE210, false_v},            /* drop */
    [VKR_UI_ICON_WAVES] = {0xE6DE, false_v},           /* waves */
    [VKR_UI_ICON_TREE] = {0xE6DA, false_v},            /* tree */
    [VKR_UI_ICON_BUILDINGS] = {0xE102, false_v},       /* buildings */
    [VKR_UI_ICON_SELECTION] = {0xE69A, false_v},       /* selection */
    [VKR_UI_ICON_TEXT] = {0xE48A, false_v},            /* text-t */
    [VKR_UI_ICON_CODE] = {0xE1BC, false_v},            /* code */
    [VKR_UI_ICON_TERMINAL] = {0xE47E, false_v},        /* terminal */
    [VKR_UI_ICON_GIT_BRANCH] = {0xE278, false_v},      /* git-branch */
    [VKR_UI_ICON_PUZZLE] = {0xE596, false_v},          /* puzzle-piece */
    [VKR_UI_ICON_STAR] = {0xE46A, false_v},            /* star */
    [VKR_UI_ICON_STAR_FILL] = {0xE46A, true_v},        /* star */
    [VKR_UI_ICON_PENCIL_LINE] = {0xE3B2, false_v},     /* pencil-line */
    [VKR_UI_ICON_BOUNDING_BOX] = {0xE6CE, false_v},    /* bounding-box */
    [VKR_UI_ICON_VIDEO] = {0xE740, false_v},           /* video */
    [VKR_UI_ICON_SPEAKER] = {0xE44A, false_v},         /* speaker-high */
};

vkr_internal const VkrFontGlyphId *vkr_ui_font_glyph(const VkrFont *font,
                                                     uint32_t codepoint) {
  if (!font || !font->codepoint_map.data || !font->glyphs_by_id.data)
    return NULL;
  uint64_t first = 0u;
  uint64_t count = font->codepoint_map.length;
  while (count != 0u) {
    const uint64_t step = count / 2u;
    const VkrFontCodepointMapEntry *entry =
        &font->codepoint_map.data[first + step];
    if (entry->codepoint < codepoint) {
      first += step + 1u;
      count -= step + 1u;
    } else if (entry->codepoint > codepoint) {
      count = step;
    } else {
      return entry->glyph_index < font->glyphs_by_id.length
                 ? &font->glyphs_by_id.data[entry->glyph_index]
                 : NULL;
    }
  }
  return NULL;
}

/* Fits the glyph's em box (advance by ascender-descender) into `rect`. */
vkr_internal void vkr_ui_emit_icon(VkrUiSystem *system, VkrUiDrawBuffer *buffer,
                                   VkrUiIcon icon, VkrUiRect rect, Vec4 color) {
  if (icon <= VKR_UI_ICON_NONE || icon >= VKR_UI_ICON_COUNT ||
      !vkr_ui_rect_has_area(rect) || !vkr_ui_color_visible(color))
    return;
  const VkrUiIconGlyph glyph_ref = vkr_ui_icon_glyphs[icon];
  VkrFont *font = vkr_font_system_get_by_handle(
      system->fonts,
      glyph_ref.fill ? system->icon_fill_font : system->icon_font);
  const VkrFontGlyphId *glyph = vkr_ui_font_glyph(font, glyph_ref.codepoint);
  if (!font || !glyph || !glyph->has_geometry || font->atlas.id == 0u)
    return;
  const float32_t em_height = font->em_ascender - font->em_descender > 0.0f
                                  ? font->em_ascender - font->em_descender
                                  : 1.0f;
  const float32_t size = Min(rect.width, rect.height);
  const float32_t scale = size / em_height;
  const float32_t advance = glyph->advance > 0.0f ? glyph->advance : 1.0f;
  const float32_t origin_x = rect.x + (rect.width - advance * scale) * 0.5f;
  const float32_t origin_y = rect.y + (rect.height - size) * 0.5f;
  const VkrUiRect quad = {
      origin_x + glyph->plane_left * scale,
      origin_y + (font->em_ascender - glyph->plane_top) * scale,
      (glyph->plane_right - glyph->plane_left) * scale,
      (glyph->plane_top - glyph->plane_bottom) * scale,
  };
  /* Icon atlases cook a 16 px distance range at 64 px per em. */
  const float32_t screen_range = Max(size * 0.25f, 1.0f);
  (void)vkr_ui_draw_buffer_text_quad(
      buffer, quad,
      (Vec4){glyph->uv_left, glyph->uv_top, glyph->uv_right, glyph->uv_bottom},
      vkr_ui_linear_color(color),
      (VkrUiTextureRef){font->atlas.id, font->atlas.generation},
      VKR_UI_DRAW_MODE_MTSDF_TEXT, screen_range, font->mtsdf_unit_range);
}

vkr_internal float32_t vkr_ui_text_screen_range(const VkrUiText *text,
                                                const VkrFont *font) {
  const float32_t authored_size = text->config.font_size > 0.0f
                                      ? text->config.font_size
                                      : (float32_t)font->size;
  const float32_t em_size = font->em_size > 0.0f ? font->em_size : 1.0f;
  return font->sdf_distance_range * authored_size * text->content_scale /
         em_size;
}

/* `ellipsis_right`, when positive, is the pixel edge a single-line text must
 * end before: glyphs past it are dropped at a glyph boundary and an ellipsis
 * (or three periods when the font lacks U+2026) marks the cut. */
vkr_internal void vkr_ui_emit_text(VkrUiSystem *system, VkrUiDrawBuffer *buffer,
                                   VkrUiFrameNode *node, VkrUiRect content_rect,
                                   bool8_t centered, float32_t x_offset,
                                   float32_t ellipsis_right) {
  VkrUiText *text = &node->retained->text;
  VkrFont *font = text->resolved_font;
  if (!node->retained->text_live || !font || font->atlas.id == 0u ||
      text->geometry.vertex_count == 0u)
    return;
  const VkrTextBounds bounds = vkr_ui_text_get_bounds(text);
  const float32_t origin_x =
      content_rect.x + x_offset -
      (node->kind == VKR_UI_NODE_TEXT_FIELD ? node->retained->scroll_offset.x
                                            : 0.0f) +
      (centered
           ? Max(0.0f, content_rect.width - x_offset - bounds.size.x) * 0.5f
           : 0.0f);
  // Center the font's line box, not the padded atlas bounds of this string.
  // A changing label must keep its baseline and its device-pixel phase.
  float32_t top =
      content_rect.y + Max(0.0f, content_rect.height - bounds.size.y) * 0.5f;
  float32_t geometry_max_y = bounds.size.y;
  if (node->kind == VKR_UI_NODE_TEXT_FIELD) {
    top = content_rect.y - node->retained->scroll_offset.y;
    geometry_max_y =
        text->layout.baseline.y - text->bounds.ascent + text->bounds.size.y;
  } else {
    top = vkr_round_f32(top + bounds.ascent) - bounds.ascent;
  }
  const VkrUiDrawMode mode = font->type == VKR_FONT_TYPE_MTSDF
                                 ? VKR_UI_DRAW_MODE_MTSDF_TEXT
                                 : VKR_UI_DRAW_MODE_BITMAP_TEXT;
  const float32_t screen_range = mode == VKR_UI_DRAW_MODE_MTSDF_TEXT
                                     ? vkr_ui_text_screen_range(text, font)
                                     : 0.0f;
  const Vec2 unit_range =
      mode == VKR_UI_DRAW_MODE_MTSDF_TEXT ? font->mtsdf_unit_range : (Vec2){0};
  const VkrUiTextureRef atlas = {font->atlas.id, font->atlas.generation};
  float32_t cut = VKR_FLOAT_MAX;
  const VkrFontGlyphId *ellipsis = NULL;
  uint32_t ellipsis_count = 0u;
  float32_t em = 0.0f;
  if (ellipsis_right > 0.0f && text->layout.line_count == 1u &&
      mode == VKR_UI_DRAW_MODE_MTSDF_TEXT &&
      bounds.size.x > ellipsis_right - origin_x + 0.5f) {
    ellipsis = vkr_ui_font_glyph(font, 0x2026u);
    ellipsis_count = 1u;
    if (!ellipsis || !ellipsis->has_geometry) {
      ellipsis = vkr_ui_font_glyph(font, '.');
      ellipsis_count = 3u;
    }
    if (ellipsis && ellipsis->has_geometry) {
      em = vkr_ui_text_device_font_size(text);
      const float32_t limit =
          ellipsis_right - origin_x -
          ellipsis->advance * em * (float32_t)ellipsis_count;
      cut = 0.0f;
      for (uint32_t g = 0u; g < text->layout.glyphs.length; ++g) {
        const VkrTextGlyph *glyph = &text->layout.glyphs.data[g];
        const float32_t end = glyph->position.x + glyph->advance;
        if (end > limit)
          break;
        cut = end;
      }
    } else {
      ellipsis = NULL;
    }
  }
  for (uint32_t vertex = 0u; vertex + 3u < text->geometry.vertex_count;
       vertex += 4u) {
    const VkrTextVertex *quad = &text->geometry.vertices[vertex];
    float32_t min_x = quad[0].position.x;
    float32_t max_x = min_x;
    float32_t min_y = quad[0].position.y;
    float32_t max_y = min_y;
    for (uint32_t i = 1u; i < 4u; ++i) {
      min_x = Min(min_x, quad[i].position.x);
      max_x = Max(max_x, quad[i].position.x);
      min_y = Min(min_y, quad[i].position.y);
      max_y = Max(max_y, quad[i].position.y);
    }
    if ((min_x + max_x) * 0.5f >= cut)
      continue;
    const VkrUiRect rect = {
        .x = origin_x + min_x,
        .y = top + geometry_max_y - max_y,
        .width = max_x - min_x,
        .height = max_y - min_y,
    };
    if (!vkr_ui_rect_has_area(rect))
      continue;
    const Vec4 uv = {quad[0].texcoord.x, quad[1].texcoord.y, quad[1].texcoord.x,
                     quad[0].texcoord.y};
    (void)vkr_ui_draw_buffer_text_quad(buffer, rect, uv, quad[0].color, atlas,
                                       mode, screen_range, unit_range);
  }
  const float32_t baseline = top + bounds.ascent;
  for (uint32_t i = 0u; ellipsis && i < ellipsis_count; ++i) {
    const float32_t pen =
        origin_x + cut + ellipsis->advance * em * (float32_t)i;
    const VkrUiRect rect = {
        pen + ellipsis->plane_left * em,
        baseline - ellipsis->plane_top * em,
        (ellipsis->plane_right - ellipsis->plane_left) * em,
        (ellipsis->plane_top - ellipsis->plane_bottom) * em,
    };
    (void)vkr_ui_draw_buffer_text_quad(
        buffer, rect,
        (Vec4){ellipsis->uv_left, ellipsis->uv_top, ellipsis->uv_right,
               ellipsis->uv_bottom},
        text->geometry.vertices[0].color, atlas, mode, screen_range,
        unit_range);
  }
}

vkr_internal VkrUiRect vkr_ui_uniform_inset(VkrUiRect rect, float32_t inset) {
  return vkr_ui_rect_inset(rect, (VkrUiEdges){inset, inset, inset, inset});
}

#define VKR_UI_CHECKBOX_RADIUS_PT 3.0f

/* A checkbox's square at the leading edge of its content, centered on it
   vertically; its label follows. */
vkr_internal VkrUiRect vkr_ui_checkbox_box(VkrUiRect content, float32_t scale) {
  const float32_t size = Min(content.height, 15.0f * scale);
  return (VkrUiRect){content.x, content.y + (content.height - size) * 0.5f,
                     size, size};
}

vkr_internal uint32_t vkr_ui_bezier_segments(const VkrUiFrameNode *node) {
  const bool8_t straight =
      node->bezier_points[0].x == node->bezier_points[1].x &&
      node->bezier_points[0].y == node->bezier_points[1].y &&
      node->bezier_points[2].x == node->bezier_points[3].x &&
      node->bezier_points[2].y == node->bezier_points[3].y;
  return straight ? 1u : 24u;
}

/* Hue-preserving brightness change for derived hover and press states. */
vkr_internal Vec4 vkr_ui_color_scale(Vec4 color, float32_t factor) {
  return (Vec4){vkr_clamp_f32(color.x * factor, 0.0f, 1.0f),
                vkr_clamp_f32(color.y * factor, 0.0f, 1.0f),
                vkr_clamp_f32(color.z * factor, 0.0f, 1.0f), color.w};
}

/* Eases a widget's fill and border toward its hover, pressed, and focus
 * colors. Derived states apply only when the style leaves them unset. */
vkr_internal void vkr_ui_node_state_colors(const VkrUiFrameNode *node,
                                           Vec4 *background,
                                           Vec4 *border_color) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiRetainedState *retained = node->retained;
  if (node->kind == VKR_UI_NODE_BUTTON) {
    /* Transparent "ghost" buttons, such as list rows, gain a subtle fill. */
    const bool8_t ghost = background->w <= 0.0f;
    Vec4 hover = node->style.hover_background_color;
    if (hover.w == 0.0f)
      hover = ghost ? theme->row_hover : vkr_ui_color_scale(*background, 1.15f);
    Vec4 active = node->style.active_background_color;
    if (active.w == 0.0f)
      active = ghost ? vkr_ui_color_alpha(theme->row_hover,
                                          theme->row_hover.w * 0.6f)
                     : vkr_ui_color_scale(*background, 0.8f);
    if (hover.w < 0.0f)
      hover = *background;
    if (active.w < 0.0f)
      active = hover;
    if (ghost) {
      *background = hover;
      background->w *= retained->hover_t;
      if (retained->active_t > 0.0f)
        *background = vkr_ui_color_mix(*background, active, retained->active_t);
    } else if (node->disabled) {
      background->w *= 0.55f;
      border_color->w *= 0.55f;
    } else {
      *background = vkr_ui_color_mix(*background, hover, retained->hover_t);
      *background = vkr_ui_color_mix(*background, active, retained->active_t);
    }
  } else if (node->kind == VKR_UI_NODE_TEXT_FIELD && !node->disabled) {
    *border_color = vkr_ui_color_mix(*border_color, theme->border_strong,
                                     retained->hover_t);
    *border_color =
        vkr_ui_color_mix(*border_color, theme->focus_ring, retained->focus_t);
  }
}

/* Overflowing scroll areas show a thin thumb over their right edge. */
vkr_internal void vkr_ui_emit_scrollbar(const VkrUiSystem *system,
                                        const VkrUiFrameNode *node,
                                        VkrUiDrawBuffer *buffer) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiRetainedState *retained = node->retained;
  VkrUiRect gutter;
  VkrUiRect bar;
  if (!vkr_ui_scroll_thumb(retained, &node->style, system->content_scale,
                           &gutter, &bar))
    return;
  const float32_t radius = bar.width * 0.5f;
  const Vec4 radii = {radius, radius, radius, radius};
  const float32_t emphasis =
      retained->scroll_dragging ? 1.0f : retained->hover_t;
  vkr_ui_emit_rect(
      buffer, bar,
      vkr_ui_color_mix(vkr_ui_color_alpha(theme->border_strong, 0.7f),
                       theme->text_secondary, emphasis),
      radii);
}

// =============================================================================
// Code view
// =============================================================================

#define VKR_UI_CODE_TAB_COLUMNS 4u

/* One UTF-8 codepoint at `*offset`, advancing past it; malformed bytes read
 * as U+FFFD, one byte each. */
vkr_internal uint32_t vkr_ui_code_decode(String8 text, uint32_t *offset) {
  const uint8_t *bytes = text.str + *offset;
  const uint64_t remaining = text.length - *offset;
  const uint8_t lead = bytes[0];
  uint32_t length = lead < 0x80u   ? 1u
                    : lead < 0xe0u ? 2u
                    : lead < 0xf0u ? 3u
                                   : 4u;
  if (lead >= 0x80u && lead < 0xc0u)
    length = 1u;
  if (length > remaining) {
    *offset += 1u;
    return 0xfffdu;
  }
  uint32_t codepoint = length == 1u   ? lead
                       : length == 2u ? lead & 0x1fu
                       : length == 3u ? lead & 0x0fu
                                      : lead & 0x07u;
  for (uint32_t i = 1u; i < length; ++i)
    codepoint = (codepoint << 6u) | (bytes[i] & 0x3fu);
  *offset += length;
  return lead >= 0x80u && lead < 0xc0u ? 0xfffdu : codepoint;
}

/* Display column of byte `offset`, with tabs expanded. */
vkr_internal uint32_t vkr_ui_code_column(String8 text, uint32_t offset) {
  uint32_t column = 0u;
  uint32_t at = 0u;
  while (at < offset && at < text.length) {
    const uint32_t codepoint = vkr_ui_code_decode(text, &at);
    column = codepoint == '\t' ? (column / VKR_UI_CODE_TAB_COLUMNS + 1u) *
                                     VKR_UI_CODE_TAB_COLUMNS
                               : column + 1u;
  }
  return column;
}

vkr_internal VkrFont *vkr_ui_code_font(VkrUiSystem *system,
                                       VkrFontHandle handle) {
  return vkr_font_system_get_by_handle(
      system->fonts, handle.id ? handle : system->default_font);
}

Vec2 vkr_ui_code_cell_size(VkrUiSystem *system, VkrFontHandle handle,
                           float32_t font_size_pt) {
  VkrFont *font = system ? vkr_ui_code_font(system, handle) : NULL;
  if (!font || font_size_pt <= 0.0f)
    return (Vec2){0};
  const VkrFontGlyphId *glyph = vkr_ui_font_glyph(font, '0');
  const float32_t advance = glyph ? glyph->advance : 0.5f;
  const float32_t line = font->em_ascender - font->em_descender > 0.0f
                             ? font->em_ascender - font->em_descender
                             : 1.2f;
  return (Vec2){advance * font_size_pt, line * font_size_pt};
}

bool8_t vkr_ui_code_view(VkrUiSystem *system, String8 id_label,
                         const VkrUiCodeView *view,
                         const VkrUiWidgetConfig *source_config,
                         VkrUiId *out_id) {
  if (out_id)
    *out_id = VKR_UI_ID_NONE;
  if (!system || !system->frame_open || !view ||
      (view->line_count && !view->lines) || !(view->font_size_pt > 0.0f) ||
      !(view->line_height_pt > 0.0f))
    return false_v;
  const VkrUiWidgetConfig fallback = vkr_ui_widget_config_default();
  const VkrUiWidgetConfig *config = source_config ? source_config : &fallback;
  const VkrUiId id = vkr_ui_id_stack_widget_label(&system->id_stack, id_label);
  if (out_id)
    *out_id = id;
  const uint32_t index = vkr_ui_add_node(system, id, VKR_UI_NODE_CODE,
                                         config->placement, &config->style);
  if (index == VKR_UI_NODE_NONE)
    return false_v;
  VkrUiFrameNode *node = &system->frame_nodes[index];
  node->cursor = VKR_WINDOW_CURSOR_IBEAM;
  node->disabled = config->disabled;
  (void)vkr_ui_interact_input(system, node, true_v);

  /* Copy lines, text and spans: drawing resolves after the caller's frame
     data may be gone. The hash drives retained tile damage. */
  /* Spans hold a Vec4; each line's run starts at the span alignment. */
  const uintptr_t span_align = AlignOf(VkrUiCodeSpan);
  uint64_t bytes = (uint64_t)view->line_count * sizeof(VkrUiCodeLine);
  for (uint32_t i = 0; i < view->line_count; ++i)
    bytes += view->lines[i].text.length +
             (uint64_t)view->lines[i].span_count * sizeof(VkrUiCodeSpan) +
             span_align;
  uint8_t *storage = bytes ? vkr_allocator_alloc(system->frame_allocator, bytes,
                                                 VKR_ALLOCATOR_MEMORY_TAG_ARRAY)
                           : NULL;
  if (bytes && !storage)
    return false_v;
  VkrUiCodeLine *lines = (VkrUiCodeLine *)storage;
  uint8_t *cursor = storage + (uint64_t)view->line_count * sizeof(*lines);
  /* Field by field: struct padding is not part of the view. */
  const float32_t metrics[4] = {view->font_size_pt, view->line_height_pt,
                                view->gutter_width_pt, view->scroll_x_pt};
  const Vec4 colors[5] = {view->text_color, view->gutter_text_color,
                          view->selection_color, view->current_line_color,
                          view->caret_color};
  uint64_t hash =
      vkr_ui_hash_bytes(UINT64_C(1469598103934665603), &view->line_count,
                        sizeof(view->line_count));
  hash = vkr_ui_hash_bytes(hash, &view->font, sizeof(view->font));
  hash = vkr_ui_hash_bytes(hash, metrics, sizeof(metrics));
  hash = vkr_ui_hash_bytes(hash, colors, sizeof(colors));
  uint32_t glyphs = 0u;
  for (uint32_t i = 0; i < view->line_count; ++i) {
    const VkrUiCodeLine *from = &view->lines[i];
    VkrUiCodeLine *line = &lines[i];
    *line = *from;
    cursor =
        (uint8_t *)(((uintptr_t)cursor + span_align - 1u) & ~(span_align - 1u));
    if (from->span_count && from->spans) {
      VkrUiCodeSpan *spans = (VkrUiCodeSpan *)cursor;
      MemCopy(spans, from->spans, from->span_count * sizeof(*spans));
      line->spans = spans;
      cursor += from->span_count * sizeof(*spans);
      /* Field by field: the padding before `color` is not part of it. */
      for (uint32_t k = 0; k < from->span_count; ++k) {
        hash = vkr_ui_hash_bytes(hash, &spans[k].start, sizeof(spans[k].start));
        hash =
            vkr_ui_hash_bytes(hash, &spans[k].length, sizeof(spans[k].length));
        hash = vkr_ui_hash_bytes(hash, &spans[k].color, sizeof(float32_t) * 4u);
      }
    } else {
      line->spans = NULL;
      line->span_count = 0u;
    }
    if (from->text.length) {
      MemCopy(cursor, from->text.str, from->text.length);
      line->text = (String8){.str = cursor, .length = from->text.length};
      cursor += from->text.length;
      hash = vkr_ui_hash_bytes(hash, line->text.str, line->text.length);
    }
    const uint32_t marks[6] = {line->number,          line->selection_start,
                               line->selection_end,   line->caret,
                               line->underline_start, line->underline_end};
    hash = vkr_ui_hash_bytes(hash, marks, sizeof(marks));
    hash = vkr_ui_hash_bytes(hash, &line->underline_color,
                             sizeof(line->underline_color));
    hash = vkr_ui_hash_bytes(hash, &line->marker_color,
                             sizeof(line->marker_color));
    hash = vkr_ui_hash_bytes(hash, &line->current, sizeof(line->current));
    glyphs += (uint32_t)Min(line->text.length, (uint64_t)UINT32_MAX / 4u) + 12u;
  }
  node->code = *view;
  node->code.lines = lines;
  node->code_hash = hash;
  node->code_glyphs = glyphs;
  const bool8_t focused = system->focused_id == id && !node->disabled;
  if (focused)
    system->focused_is_text = true_v;
  return focused;
}

/* One glyph at pen `x` on `baseline`; a glyph the font lacks draws as '?'. */
vkr_internal void vkr_ui_code_glyph(VkrUiDrawBuffer *buffer,
                                    const VkrFont *font, uint32_t codepoint,
                                    float32_t x, float32_t baseline,
                                    float32_t em, Vec4 color,
                                    float32_t screen_range) {
  const VkrFontGlyphId *glyph = vkr_ui_font_glyph(font, codepoint);
  if (!glyph || !glyph->has_geometry)
    glyph = vkr_ui_font_glyph(font, '?');
  if (!glyph || !glyph->has_geometry)
    return;
  const VkrUiRect rect = {x + glyph->plane_left * em,
                          baseline - glyph->plane_top * em,
                          (glyph->plane_right - glyph->plane_left) * em,
                          (glyph->plane_top - glyph->plane_bottom) * em};
  (void)vkr_ui_draw_buffer_text_quad(
      buffer, rect,
      (Vec4){glyph->uv_left, glyph->uv_top, glyph->uv_right, glyph->uv_bottom},
      color, (VkrUiTextureRef){font->atlas.id, font->atlas.generation},
      VKR_UI_DRAW_MODE_MTSDF_TEXT, screen_range, font->mtsdf_unit_range);
}

vkr_internal void vkr_ui_emit_code(VkrUiSystem *system, VkrUiDrawBuffer *buffer,
                                   const VkrUiFrameNode *node,
                                   VkrUiRect content) {
  const VkrUiCodeView *view = &node->code;
  VkrFont *font = vkr_ui_code_font(system, view->font);
  if (!font || font->atlas.id == 0u || font->type != VKR_FONT_TYPE_MTSDF)
    return;
  const float32_t scale = system->content_scale;
  const float32_t em = view->font_size_pt * scale;
  const float32_t line_height = vkr_round_f32(view->line_height_pt * scale);
  const Vec2 cell =
      vkr_ui_code_cell_size(system, view->font, view->font_size_pt);
  const float32_t advance = cell.x * scale;
  const float32_t gutter = vkr_round_f32(view->gutter_width_pt * scale);
  const float32_t pad = vkr_round_f32(4.0f * scale);
  const float32_t em_size = font->em_size > 0.0f ? font->em_size : 1.0f;
  const float32_t screen_range =
      font->sdf_distance_range * view->font_size_pt * scale / em_size;
  const float32_t text_left = content.x + gutter + pad;
  const float32_t origin_x = text_left - view->scroll_x_pt * scale;
  const VkrUiRect text_area = {content.x + gutter, content.y,
                               Max(0.0f, content.width - gutter),
                               content.height};
  const Vec4 text_color = vkr_ui_linear_color(view->text_color);
  const Vec4 gutter_color = vkr_ui_linear_color(view->gutter_text_color);
  const Vec4 selection = vkr_ui_linear_color(view->selection_color);
  const Vec4 current = vkr_ui_linear_color(view->current_line_color);
  const Vec4 caret = vkr_ui_linear_color(view->caret_color);
  const float32_t glyph_box = (font->em_ascender - font->em_descender) * em;
  for (uint32_t i = 0; i < view->line_count; ++i) {
    const VkrUiCodeLine *line = &view->lines[i];
    const float32_t top = content.y + line_height * (float32_t)i;
    if (top >= content.y + content.height)
      break;
    const float32_t baseline = vkr_round_f32(
        top + (line_height - glyph_box) * 0.5f + font->em_ascender * em);
    if (line->current)
      vkr_ui_emit_rect(buffer,
                       (VkrUiRect){content.x, top, content.width, line_height},
                       current, (Vec4){0});
    if (line->marker_color.w > 0.0f) {
      const float32_t size = vkr_round_f32(line_height * 0.4f);
      vkr_ui_emit_rect(
          buffer,
          (VkrUiRect){content.x + pad, top + (line_height - size) * 0.5f, size,
                      size},
          vkr_ui_linear_color(line->marker_color),
          (Vec4){size * 0.5f, size * 0.5f, size * 0.5f, size * 0.5f});
    }
    if (line->number) {
      char digits[12];
      const int32_t count =
          string_format(digits, sizeof(digits), "%u", line->number);
      float32_t x = content.x + gutter - pad - advance * (float32_t)count;
      for (int32_t d = 0; d < count; ++d, x += advance)
        vkr_ui_code_glyph(buffer, font, (uint32_t)digits[d], x, baseline, em,
                          gutter_color, screen_range);
    }
    if (!vkr_ui_draw_buffer_push_clip(
            buffer, vkr_ui_rect_intersect(text_area, node->clip)))
      continue;
    if (line->selection_end > line->selection_start) {
      const float32_t x0 =
          origin_x + advance * (float32_t)vkr_ui_code_column(
                                   line->text, line->selection_start);
      const bool8_t breaks = line->selection_end > line->text.length;
      const float32_t x1 =
          origin_x + advance * ((float32_t)vkr_ui_code_column(
                                    line->text, line->selection_end) +
                                (breaks ? 0.6f : 0.0f));
      vkr_ui_emit_rect(buffer, (VkrUiRect){x0, top, x1 - x0, line_height},
                       selection, (Vec4){0});
    }
    uint32_t span = 0u;
    uint32_t column = 0u;
    uint32_t offset = 0u;
    const float32_t right = content.x + content.width;
    while (offset < line->text.length) {
      const uint32_t at = offset;
      const uint32_t codepoint = vkr_ui_code_decode(line->text, &offset);
      const float32_t x = origin_x + advance * (float32_t)column;
      column = codepoint == '\t' ? (column / VKR_UI_CODE_TAB_COLUMNS + 1u) *
                                       VKR_UI_CODE_TAB_COLUMNS
                                 : column + 1u;
      if (x > right)
        break;
      if (codepoint <= ' ' || x + advance < text_area.x)
        continue;
      while (span < line->span_count &&
             line->spans[span].start + line->spans[span].length <= at)
        ++span;
      const bool8_t colored =
          span < line->span_count && line->spans[span].start <= at;
      vkr_ui_code_glyph(buffer, font, codepoint, x, baseline, em,
                        colored ? vkr_ui_linear_color(line->spans[span].color)
                                : text_color,
                        screen_range);
    }
    if (line->underline_end > line->underline_start) {
      const float32_t x0 =
          origin_x + advance * (float32_t)vkr_ui_code_column(
                                   line->text, line->underline_start);
      const float32_t x1 =
          origin_x + advance * (float32_t)vkr_ui_code_column(
                                   line->text, line->underline_end);
      const float32_t thickness = Max(1.0f, vkr_round_f32(scale));
      vkr_ui_emit_rect(buffer,
                       (VkrUiRect){x0, baseline + thickness * 2.0f,
                                   Max(x1 - x0, advance), thickness},
                       vkr_ui_linear_color(line->underline_color), (Vec4){0});
    }
    if (line->caret != UINT32_MAX) {
      const float32_t x = origin_x + advance * (float32_t)vkr_ui_code_column(
                                                   line->text, line->caret);
      vkr_ui_emit_rect(buffer,
                       (VkrUiRect){vkr_round_f32(x), top + scale,
                                   Max(1.0f, vkr_round_f32(2.0f * scale)),
                                   line_height - 2.0f * scale},
                       caret, (Vec4){0});
    }
    (void)vkr_ui_draw_buffer_pop_clip(buffer);
  }
}

/* Plot columns across `width_px` pixels: one per sample while each keeps a
 * point, and no more than the bar budget allows for the plot's stack. A
 * point, not a pixel: a high-density display would double the geometry for
 * bars the eye cannot tell apart. */
vkr_internal uint32_t vkr_ui_plot_column_count(const VkrUiSystem *system,
                                               const VkrUiPlotData *plot,
                                               float32_t width_px) {
  const float32_t pixels =
      vkr_floor_f32(width_px / Max(system->content_scale, 1.0f));
  uint32_t columns = plot->sample_count;
  if (pixels < (float32_t)columns) {
    columns = pixels >= 1.0f ? (uint32_t)pixels : 1u;
  }
  const uint32_t budget = VKR_UI_PLOT_BAR_BUDGET / Max(plot->bar_count, 1u);
  return Max(Min(columns, budget), 1u);
}

/* First sample of `column`; `columns` itself gives the sample count. Every
 * column holds at least one sample because columns <= samples. */
vkr_internal uint32_t vkr_ui_plot_column_start(const VkrUiPlotData *plot,
                                               uint32_t column,
                                               uint32_t columns) {
  return (uint32_t)((uint64_t)column * plot->sample_count / columns);
}

/* The column holding `sample`: the last whose start is at or before it. */
vkr_internal uint32_t vkr_ui_plot_sample_column(const VkrUiPlotData *plot,
                                                uint32_t sample,
                                                uint32_t columns) {
  return (uint32_t)((((uint64_t)sample + 1u) * columns - 1u) /
                    plot->sample_count);
}

/* The tallest sample of a column, so spikes survive; ties keep the first. */
vkr_internal uint32_t vkr_ui_plot_column_sample(const VkrUiPlotData *plot,
                                                uint32_t column,
                                                uint32_t columns) {
  const uint32_t start = vkr_ui_plot_column_start(plot, column, columns);
  const uint32_t end = vkr_ui_plot_column_start(plot, column + 1u, columns);
  uint32_t best = start;
  for (uint32_t sample = start + 1u; sample < end; ++sample) {
    if (plot->totals[sample] > plot->totals[best]) {
      best = sample;
    }
  }
  return best;
}

vkr_internal uint32_t vkr_ui_plot_pointer_column(float32_t x, VkrUiRect content,
                                                 uint32_t columns) {
  const float32_t column =
      vkr_floor_f32((x - content.x) / content.width * (float32_t)columns);
  return (uint32_t)vkr_clamp_f32(column, 0.0f, (float32_t)(columns - 1u));
}

/* Left pixel edge of `column`, rounded so neighbouring columns abut. */
vkr_internal float32_t vkr_ui_plot_column_x(VkrUiRect content, uint32_t column,
                                            uint32_t columns) {
  return vkr_round_f32(content.x +
                       content.width * (float32_t)column / (float32_t)columns);
}

/* Y pixel of `value` in the content box, clamped to [minimum, maximum]. */
vkr_internal float32_t vkr_ui_plot_y(float32_t value, float32_t minimum,
                                     float32_t maximum, VkrUiRect content) {
  const float32_t t =
      (vkr_clamp_f32(value, minimum, maximum) - minimum) / (maximum - minimum);
  return content.y + content.height * (1.0f - t);
}

vkr_internal float32_t vkr_ui_plot_finite(float32_t value) {
  return vkr_is_finite_f32(value) ? value : 0.0f;
}

vkr_internal bool8_t vkr_ui_plot_series_valid(const VkrUiPlotSeries *series,
                                              uint32_t count,
                                              uint32_t capacity) {
  if (count > capacity || (count && !series)) {
    return false_v;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    if (!series[i].values) {
      return false_v;
    }
  }
  return true_v;
}

/* Copies the caller's plot into frame memory, sanitized; NULL when the plot
 * is invalid or memory runs out. Samples past the capacity are dropped and
 * reported through `out_truncated`. */
vkr_internal VkrUiPlotData *vkr_ui_plot_copy(VkrUiSystem *system,
                                             const VkrUiPlotConfig *source,
                                             bool8_t *out_truncated) {
  if (source->sample_count == 0u || !vkr_is_finite_f32(source->y_min) ||
      !vkr_is_finite_f32(source->y_max) || source->y_max <= source->y_min ||
      !vkr_ui_plot_series_valid(source->bars, source->bar_count,
                                VKR_UI_PLOT_BAR_SERIES_CAPACITY) ||
      !vkr_ui_plot_series_valid(source->lines, source->line_count,
                                VKR_UI_PLOT_LINE_SERIES_CAPACITY) ||
      source->reference_count > VKR_UI_PLOT_REFERENCE_CAPACITY ||
      (source->reference_count && !source->references) ||
      source->palette_count > VKR_UI_PLOT_PALETTE_CAPACITY ||
      (source->palette_count && !source->palette)) {
    return NULL;
  }

  const uint32_t count = Min(source->sample_count, VKR_UI_PLOT_SAMPLE_CAPACITY);
  *out_truncated = source->sample_count > count;
  const bool8_t indexed = source->bar_colors && source->bar_count > 0u;
  const uint64_t value_rows =
      (uint64_t)source->bar_count + source->line_count + 1u;
  const uint64_t bytes = sizeof(VkrUiPlotData) +
                         value_rows * count * sizeof(float32_t) +
                         (indexed ? count : 0u);
  uint8_t *storage = vkr_allocator_alloc(system->frame_allocator, bytes,
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!storage) {
    return NULL;
  }

  VkrUiPlotData *plot = (VkrUiPlotData *)storage;
  float32_t *values = (float32_t *)(storage + sizeof(*plot));
  *plot = (VkrUiPlotData){
      .sample_count = count,
      .y_min = source->y_min,
      .y_max = source->y_max,
      .bars = values,
      .bar_count = source->bar_count,
      .palette_count = source->palette_count,
      .totals = values + (uint64_t)source->bar_count * count,
      .lines = values + ((uint64_t)source->bar_count + 1u) * count,
      .line_count = source->line_count,
      .line_width_pt = vkr_is_finite_f32(source->line_width_pt) &&
                               source->line_width_pt > 0.0f
                           ? source->line_width_pt
                           : 1.5f,
      .cursor = source->cursor < count ? source->cursor : VKR_UI_PLOT_NONE,
      .cursor_color = source->cursor_color,
      .range_first = VKR_UI_PLOT_NONE,
      .range_last = VKR_UI_PLOT_NONE,
      .range_color = source->range_color,
      .hovered = VKR_UI_PLOT_NONE,
      .hover_color = source->hover_color,
  };
  if (source->range_first <= source->range_last &&
      source->range_first < count) {
    plot->range_first = source->range_first;
    plot->range_last = Min(source->range_last, count - 1u);
  }

  /* Bars: non-finite values count as zero. A sample's total is the extent of
   * its stack above and below zero. */
  MemZero(plot->totals, (uint64_t)count * sizeof(*plot->totals));
  for (uint32_t series = 0u; series < source->bar_count; ++series) {
    const float32_t *from = source->bars[series].values;
    float32_t *to = plot->bars + (uint64_t)series * count;
    plot->bar_series_colors[series] = source->bars[series].color;
    for (uint32_t i = 0u; i < count; ++i) {
      to[i] = vkr_ui_plot_finite(from[i]);
      plot->totals[i] += vkr_abs_f32(to[i]);
    }
  }
  for (uint32_t line = 0u; line < source->line_count; ++line) {
    const VkrUiPlotSeries *series = &source->lines[line];
    float32_t *to = plot->lines + (uint64_t)line * count;
    for (uint32_t i = 0u; i < count; ++i) {
      to[i] = vkr_ui_plot_finite(series->values[i]);
    }
    plot->line_colors[line] = series->color;
    const bool8_t own_range = vkr_is_finite_f32(series->y_min) &&
                              vkr_is_finite_f32(series->y_max) &&
                              series->y_max > series->y_min;
    plot->line_ranges[line] = own_range ? (Vec2){series->y_min, series->y_max}
                                        : (Vec2){source->y_min, source->y_max};
  }
  /* Without bars, the first line picks each column's sample. */
  if (source->bar_count == 0u && source->line_count > 0u) {
    MemCopy(plot->totals, plot->lines, (uint64_t)count * sizeof(*plot->totals));
  }
  if (indexed) {
    plot->bar_colors = (uint8_t *)(values + value_rows * count);
    MemCopy(plot->bar_colors, source->bar_colors, count);
  }
  if (source->palette_count) {
    MemCopy(plot->palette, source->palette,
            (uint64_t)source->palette_count * sizeof(*plot->palette));
  }
  for (uint32_t i = 0u; i < source->reference_count; ++i) {
    const VkrUiPlotReference *reference = &source->references[i];
    if (!vkr_is_finite_f32(reference->value)) {
      continue;
    }
    plot->references[plot->reference_count++] = (VkrUiPlotReference){
        .value = reference->value,
        .color = reference->color,
        .width_pt =
            vkr_is_finite_f32(reference->width_pt) && reference->width_pt > 0.0f
                ? reference->width_pt
                : 1.0f,
        .dashed = reference->dashed,
    };
  }
  return plot;
}

/* What the plot's runs draw, field by field: struct padding is not part of
 * the plot. The cursor, range and hover marks are hashed with the node. */
vkr_internal uint64_t vkr_ui_plot_hash(const VkrUiPlotData *plot) {
  const uint32_t count = plot->sample_count;
  const uint32_t counts[5] = {count, plot->bar_count, plot->line_count,
                              plot->palette_count, plot->reference_count};
  const float32_t metrics[3] = {plot->y_min, plot->y_max, plot->line_width_pt};
  uint64_t hash =
      vkr_ui_hash_bytes(UINT64_C(1469598103934665603), counts, sizeof(counts));
  hash = vkr_ui_hash_bytes(hash, metrics, sizeof(metrics));
  hash = vkr_ui_hash_bytes(hash, plot->bar_series_colors,
                           plot->bar_count * sizeof(Vec4));
  hash = vkr_ui_hash_bytes(hash, plot->palette,
                           plot->palette_count * sizeof(Vec4));
  hash = vkr_ui_hash_bytes(hash, plot->line_colors,
                           plot->line_count * sizeof(Vec4));
  hash = vkr_ui_hash_bytes(hash, plot->line_ranges,
                           plot->line_count * sizeof(Vec2));
  hash = vkr_ui_hash_bytes(
      hash, plot->bars, (uint64_t)plot->bar_count * count * sizeof(float32_t));
  hash =
      vkr_ui_hash_bytes(hash, plot->lines,
                        (uint64_t)plot->line_count * count * sizeof(float32_t));
  const bool8_t indexed = plot->bar_colors != NULL;
  hash = vkr_ui_hash_bytes(hash, &indexed, sizeof(indexed));
  if (indexed) {
    hash = vkr_ui_hash_bytes(hash, plot->bar_colors, count);
  }
  for (uint32_t i = 0u; i < plot->reference_count; ++i) {
    const VkrUiPlotReference *reference = &plot->references[i];
    hash = vkr_ui_hash_bytes(hash, &reference->value, sizeof(reference->value));
    hash = vkr_ui_hash_bytes(hash, &reference->color, sizeof(reference->color));
    hash = vkr_ui_hash_bytes(hash, &reference->width_pt,
                             sizeof(reference->width_pt));
    hash =
        vkr_ui_hash_bytes(hash, &reference->dashed, sizeof(reference->dashed));
  }
  return hash;
}

/* Hover, click and brush from the previous frame's layout, as other
 * widgets hit-test. */
vkr_internal void vkr_ui_plot_input(VkrUiSystem *system, VkrUiFrameNode *node,
                                    bool8_t was_active, bool8_t released_here,
                                    VkrUiPlotResult *result) {
  VkrUiRetainedState *retained = node->retained;
  const VkrUiPlotData *plot = node->plot;
  const VkrUiRect content =
      vkr_ui_style_content_rect(retained->last_rect, &node->style);
  if (!vkr_ui_rect_has_area(content)) {
    return;
  }

  const uint32_t columns =
      vkr_ui_plot_column_count(system, plot, content.width);
  const uint32_t column =
      vkr_ui_plot_pointer_column((float32_t)system->mouse_x, content, columns);
  if (node->hovered && !node->disabled) {
    result->hovered = vkr_ui_plot_column_sample(plot, column, columns);
  }

  const bool8_t held = system->active_id == node->id;
  const bool8_t released = was_active && !held && system->mouse_released;
  if (held && !was_active) {
    int32_t press_x = system->mouse_x;
    int32_t press_y = system->mouse_y;
    input_get_button_press_position(system->input, BUTTON_LEFT, &press_x,
                                    &press_y);
    retained->plot_press_x = (float32_t)press_x;
    retained->plot_press_column =
        vkr_ui_plot_pointer_column((float32_t)press_x, content, columns);
    retained->plot_dragging = false_v;
  }
  if (!held && !released) {
    return;
  }

  const float32_t threshold = 3.0f * system->content_scale;
  if (vkr_abs_f32((float32_t)system->mouse_x - retained->plot_press_x) >=
      threshold) {
    retained->plot_dragging = true_v;
  }
  /* A resize while held can leave fewer columns than at the press. */
  const uint32_t press_column = Min(retained->plot_press_column, columns - 1u);
  if (retained->plot_dragging) {
    const uint32_t first = Min(press_column, column);
    const uint32_t last = Max(press_column, column);
    result->brush_first = vkr_ui_plot_column_start(plot, first, columns);
    result->brush_last =
        vkr_ui_plot_column_start(plot, last + 1u, columns) - 1u;
    result->brushing = held;
    result->brushed = released;
  } else if (released && released_here) {
    result->clicked = vkr_ui_plot_column_sample(plot, press_column, columns);
  }
  if (released) {
    retained->plot_dragging = false_v;
  }
}

VkrUiPlotResult vkr_ui_plot(VkrUiSystem *system, String8 id_label,
                            const VkrUiPlotConfig *plot,
                            const VkrUiWidgetConfig *source_config) {
  VkrUiPlotResult result = {
      .hovered = VKR_UI_PLOT_NONE,
      .clicked = VKR_UI_PLOT_NONE,
      .brush_first = VKR_UI_PLOT_NONE,
      .brush_last = VKR_UI_PLOT_NONE,
  };
  if (!system || !system->frame_open || !plot) {
    return result;
  }

  const VkrUiWidgetConfig fallback = vkr_ui_widget_config_default();
  const VkrUiWidgetConfig *config = source_config ? source_config : &fallback;
  const VkrUiId id = vkr_ui_id_stack_widget_label(&system->id_stack, id_label);
  result.id = id;
  const uint32_t index = vkr_ui_add_node(system, id, VKR_UI_NODE_PLOT,
                                         config->placement, &config->style);
  if (index == VKR_UI_NODE_NONE) {
    return result;
  }

  VkrUiFrameNode *node = &system->frame_nodes[index];
  const float32_t scale = system->content_scale;
  node->intrinsic_size = vkr_ui_style_clamp_size(
      (Vec2){Max(node->style.min_size_px.x, 120.0f * scale),
             Max(node->style.min_size_px.y, 48.0f * scale)},
      &node->style);
  node->plot = vkr_ui_plot_copy(system, plot, &result.truncated);
  if (!node->plot) {
    return result;
  }

  node->disabled = config->disabled;
  node->tooltip = config->tooltip;
  node->cursor = config->cursor;
  const bool8_t was_active = system->active_id == id;
  const bool8_t released_here = vkr_ui_interact(system, node, false_v);
  vkr_ui_plot_input(system, node, was_active, released_here, &result);
  node->plot->hovered = result.hovered;
  node->plot->hash = vkr_ui_plot_hash(node->plot);
  return result;
}

/* A run's fingerprint: the plot's content, the scale its pixels follow and
 * which of the plot's runs it is. */
vkr_internal uint64_t vkr_ui_plot_run_hash(const VkrUiSystem *system,
                                           const VkrUiPlotData *plot,
                                           uint32_t layer) {
  uint64_t hash = vkr_ui_hash_bytes(plot->hash, &system->content_scale,
                                    sizeof(system->content_scale));
  return vkr_ui_hash_bytes(hash, &layer, sizeof(layer));
}

/* Each column's tallest sample as one bar, stacked by series: positive
 * values upward from zero, negative ones downward. Bars at least four
 * pixels wide keep a one-pixel gap. One rect run draws them all. */
vkr_internal void vkr_ui_emit_plot_bars(VkrUiSystem *system,
                                        VkrUiDrawBuffer *buffer,
                                        const VkrUiPlotData *plot,
                                        VkrUiRect content, uint32_t columns) {
  VkrUiDrawRect *rects =
      vkr_allocator_alloc(system->frame_allocator,
                          (uint64_t)columns * plot->bar_count * sizeof(*rects),
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!rects) {
    return;
  }

  Vec4 series_colors[VKR_UI_PLOT_BAR_SERIES_CAPACITY];
  Vec4 palette[VKR_UI_PLOT_PALETTE_CAPACITY];
  for (uint32_t i = 0u; i < plot->bar_count; ++i) {
    series_colors[i] = vkr_ui_linear_color(plot->bar_series_colors[i]);
  }
  for (uint32_t i = 0u; i < plot->palette_count; ++i) {
    palette[i] = vkr_ui_linear_color(plot->palette[i]);
  }

  uint32_t count = 0u;
  for (uint32_t column = 0u; column < columns; ++column) {
    const uint32_t sample = vkr_ui_plot_column_sample(plot, column, columns);
    const float32_t left = vkr_ui_plot_column_x(content, column, columns);
    float32_t right = vkr_ui_plot_column_x(content, column + 1u, columns);
    if (right - left >= 4.0f) {
      right -= 1.0f;
    }
    right = Max(right, left + 1.0f);

    float32_t above = 0.0f;
    float32_t below = 0.0f;
    for (uint32_t series = 0u; series < plot->bar_count; ++series) {
      const float32_t value =
          plot->bars[(uint64_t)series * plot->sample_count + sample];
      if (value == 0.0f) {
        continue;
      }
      float32_t *edge = value > 0.0f ? &above : &below;
      const float32_t from = *edge;
      *edge += value;
      const float32_t y0 =
          vkr_round_f32(vkr_ui_plot_y(from, plot->y_min, plot->y_max, content));
      const float32_t y1 = vkr_round_f32(
          vkr_ui_plot_y(*edge, plot->y_min, plot->y_max, content));
      const float32_t top = Min(y0, y1);
      const float32_t bottom = Max(y0, y1);
      if (bottom <= top) {
        continue;
      }

      Vec4 color = series_colors[series];
      if (series == 0u && plot->bar_colors &&
          plot->bar_colors[sample] < plot->palette_count) {
        color = palette[plot->bar_colors[sample]];
      }
      rects[count++] = (VkrUiDrawRect){
          .rect = {left - content.x, top - content.y, right - left,
                   bottom - top},
          .color = color,
      };
    }
  }
  if (count) {
    (void)vkr_ui_draw_buffer_rects(buffer, content, rects, count,
                                   vkr_ui_plot_run_hash(system, plot, 0u));
  }
}

/* Reference lines inside the range, dashed 4 points on and 3 off, as one
 * rect run. */
vkr_internal void vkr_ui_emit_plot_references(VkrUiSystem *system,
                                              VkrUiDrawBuffer *buffer,
                                              const VkrUiPlotData *plot,
                                              VkrUiRect content) {
  const float32_t scale = system->content_scale;
  const float32_t dash = Max(1.0f, vkr_round_f32(4.0f * scale));
  const float32_t period = dash + Max(1.0f, vkr_round_f32(3.0f * scale));
  const uint32_t dashes = (uint32_t)vkr_ceil_f32(content.width / period);
  uint64_t capacity = 0u;
  for (uint32_t i = 0u; i < plot->reference_count; ++i) {
    capacity += plot->references[i].dashed ? dashes : 1u;
  }
  VkrUiDrawRect *rects =
      vkr_allocator_alloc(system->frame_allocator, capacity * sizeof(*rects),
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!rects) {
    return;
  }

  uint32_t count = 0u;
  for (uint32_t i = 0u; i < plot->reference_count; ++i) {
    const VkrUiPlotReference *reference = &plot->references[i];
    if (reference->value < plot->y_min || reference->value > plot->y_max) {
      continue;
    }
    const float32_t thickness =
        Max(1.0f, vkr_round_f32(reference->width_pt * scale));
    const float32_t y =
        vkr_ui_plot_y(reference->value, plot->y_min, plot->y_max, content);
    const float32_t top = vkr_round_f32(y - thickness * 0.5f) - content.y;
    const Vec4 color = vkr_ui_linear_color(reference->color);
    if (!reference->dashed) {
      rects[count++] = (VkrUiDrawRect){
          .rect = {0.0f, top, content.width, thickness},
          .color = color,
      };
      continue;
    }
    for (uint32_t d = 0u; d < dashes; ++d) {
      const float32_t x = (float32_t)d * period;
      rects[count++] = (VkrUiDrawRect){
          .rect = {x, top, Min(dash, content.width - x), thickness},
          .color = color,
      };
    }
  }
  if (count) {
    (void)vkr_ui_draw_buffer_rects(buffer, content, rects, count,
                                   vkr_ui_plot_run_hash(system, plot, 1u));
  }
}

/* Each line through its columns' highest values at their centres, as one
 * polyline run per line. */
vkr_internal void vkr_ui_emit_plot_lines(VkrUiSystem *system,
                                         VkrUiDrawBuffer *buffer,
                                         const VkrUiPlotData *plot,
                                         VkrUiRect content, uint32_t columns) {
  const uint32_t capacity = Max(columns, 2u);
  const float32_t width_px = plot->line_width_pt * system->content_scale;
  for (uint32_t line = 0u; line < plot->line_count; ++line) {
    Vec2 *points = vkr_allocator_alloc(system->frame_allocator,
                                       (uint64_t)capacity * sizeof(*points),
                                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!points) {
      return;
    }

    const float32_t *values = plot->lines + (uint64_t)line * plot->sample_count;
    const Vec2 range = plot->line_ranges[line];
    for (uint32_t column = 0u; column < columns; ++column) {
      const uint32_t start = vkr_ui_plot_column_start(plot, column, columns);
      const uint32_t end = vkr_ui_plot_column_start(plot, column + 1u, columns);
      float32_t value = values[start];
      for (uint32_t sample = start + 1u; sample < end; ++sample) {
        value = Max(value, values[sample]);
      }
      const float32_t left = vkr_ui_plot_column_x(content, column, columns);
      const float32_t right =
          vkr_ui_plot_column_x(content, column + 1u, columns);
      points[column] = (Vec2){
          (left + right) * 0.5f - content.x,
          vkr_ui_plot_y(value, range.x, range.y, content) - content.y,
      };
    }
    uint32_t count = columns;
    /* One column still draws a line across the plot. */
    if (columns == 1u) {
      points[1] = (Vec2){content.width, points[0].y};
      points[0].x = 0.0f;
      count = 2u;
    }
    (void)vkr_ui_draw_buffer_polyline(
        buffer, content, points, count, width_px,
        vkr_ui_linear_color(plot->line_colors[line]),
        vkr_ui_plot_run_hash(system, plot, 2u + line));
  }
}

/* A column-wide band at least `min_width_px` wide, centred on its column. */
vkr_internal VkrUiRect vkr_ui_plot_column_band(VkrUiRect content,
                                               uint32_t first, uint32_t last,
                                               uint32_t columns,
                                               float32_t min_width_px) {
  const float32_t left = vkr_ui_plot_column_x(content, first, columns);
  const float32_t right = vkr_ui_plot_column_x(content, last + 1u, columns);
  const float32_t width = Max(right - left, min_width_px);
  const float32_t x = vkr_round_f32((left + right - width) * 0.5f);
  return (VkrUiRect){x, content.y, width, content.height};
}

/* Back to front: range and hover bands, bars, reference lines, lines and
 * the cursor, clipped to the content box. */
/* One polyline per run of finite points whose segments share a color; a
   run ends on the point where the next color starts, so strokes meet. */
vkr_internal void vkr_ui_emit_path(VkrUiSystem *system, VkrUiDrawBuffer *buffer,
                                   const VkrUiFrameNode *node,
                                   VkrUiRect content) {
  const VkrUiPathData *path = node->path;
  if (!path || path->count < 2u || !vkr_ui_rect_has_area(content)) {
    return;
  }
  (void)system;
  uint32_t start = 0u;
  while (start + 1u < path->count) {
    if (path->colors[start] == VKR_UI_PATH_BREAK ||
        path->colors[start + 1u] == VKR_UI_PATH_BREAK) {
      ++start;
      continue;
    }
    const uint8_t color = path->colors[start];
    uint32_t end = start + 1u;
    while (end + 1u < path->count &&
           path->colors[end + 1u] != VKR_UI_PATH_BREAK &&
           path->colors[end] == color) {
      ++end;
    }
    (void)vkr_ui_draw_buffer_polyline(
        buffer, content, path->points + start, end - start + 1u, path->width_px,
        path->palette[color],
        vkr_ui_hash_bytes(path->hash, &start, sizeof(start)));
    start = end;
  }
}

/* Copies a path into frame memory in pixels, with sanitized colors. NULL
   when there is nothing to stroke or memory ran out. */
vkr_internal VkrUiPathData *vkr_ui_path_copy(VkrUiSystem *system,
                                             const VkrUiPathConfig *source) {
  if (!source || !source->points || source->count < 2u || !source->palette ||
      source->palette_count == 0u) {
    return NULL;
  }
  const uint32_t count = Min(source->count, VKR_UI_PATH_POINT_MAX);
  const uint64_t bytes = sizeof(VkrUiPathData) + sizeof(Vec2) * count + count;
  uint8_t *storage = vkr_allocator_alloc(system->frame_allocator, bytes,
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!storage) {
    return NULL;
  }
  VkrUiPathData *path = (VkrUiPathData *)storage;
  *path = (VkrUiPathData){
      .points = (Vec2 *)(storage + sizeof(VkrUiPathData)),
      .count = count,
      .palette_count = Min(source->palette_count, VKR_UI_PATH_PALETTE_CAPACITY),
      .width_px = Max(source->width_pt, 0.5f) * system->content_scale,
  };
  path->colors = (uint8_t *)(path->points + count);
  const float32_t scale = system->content_scale;
  for (uint32_t i = 0u; i < count; ++i) {
    const Vec2 point = source->points[i];
    const bool8_t finite =
        vkr_is_finite_f32(point.x) && vkr_is_finite_f32(point.y);
    path->points[i] =
        finite ? (Vec2){point.x * scale, point.y * scale} : (Vec2){0};
    const uint8_t color = source->colors ? source->colors[i] : 0u;
    path->colors[i] = !finite                       ? VKR_UI_PATH_BREAK
                      : color < path->palette_count ? color
                                                    : 0u;
  }
  for (uint32_t i = 0u; i < path->palette_count; ++i) {
    path->palette[i] = vkr_ui_linear_color(source->palette[i]);
  }
  uint64_t hash =
      vkr_ui_hash_bytes(0x9a7au, path->points, sizeof(Vec2) * (uint64_t)count);
  hash = vkr_ui_hash_bytes(hash, path->colors, count);
  hash = vkr_ui_hash_bytes(hash, path->palette, sizeof(path->palette));
  path->hash = vkr_ui_hash_bytes(hash, &path->width_px, sizeof(path->width_px));
  return path;
}

VkrUiId vkr_ui_path(VkrUiSystem *system, String8 id_label,
                    const VkrUiPathConfig *path,
                    const VkrUiWidgetConfig *source_config) {
  if (!system || !system->frame_open) {
    return VKR_UI_ID_NONE;
  }
  const VkrUiWidgetConfig fallback = vkr_ui_widget_config_default();
  const VkrUiWidgetConfig *config = source_config ? source_config : &fallback;
  const VkrUiId id = vkr_ui_id_stack_widget_label(&system->id_stack, id_label);
  const uint32_t index = vkr_ui_add_node(system, id, VKR_UI_NODE_PATH,
                                         config->placement, &config->style);
  if (index == VKR_UI_NODE_NONE) {
    return VKR_UI_ID_NONE;
  }
  VkrUiFrameNode *node = &system->frame_nodes[index];
  node->intrinsic_size =
      vkr_ui_style_clamp_size(node->style.min_size_px, &node->style);
  node->path = vkr_ui_path_copy(system, path);
  return id;
}

bool8_t vkr_ui_path_set(VkrUiSystem *system, VkrUiId id,
                        const VkrUiPathConfig *path) {
  if (!system || (!system->frame_open && !system->frame_draw_pending) ||
      id == VKR_UI_ID_NONE) {
    return false_v;
  }
  VkrUiRetainedState *retained = vkr_ui_retained_find(system, id);
  if (!retained || retained->last_seen_frame != system->frame_index) {
    return false_v;
  }
  VkrUiFrameNode *node = &system->frame_nodes[retained->frame_node_index];
  if (node->kind != VKR_UI_NODE_PATH) {
    return false_v;
  }
  node->path = vkr_ui_path_copy(system, path);
  return true_v;
}

vkr_internal void vkr_ui_emit_plot(VkrUiSystem *system, VkrUiDrawBuffer *buffer,
                                   const VkrUiFrameNode *node,
                                   VkrUiRect content) {
  const VkrUiPlotData *plot = node->plot;
  if (!plot || !vkr_ui_rect_has_area(content) ||
      !vkr_ui_draw_buffer_push_clip(buffer, content)) {
    return;
  }

  const float32_t scale = system->content_scale;
  const uint32_t columns =
      vkr_ui_plot_column_count(system, plot, content.width);
  if (plot->range_first != VKR_UI_PLOT_NONE) {
    const uint32_t first =
        vkr_ui_plot_sample_column(plot, plot->range_first, columns);
    const uint32_t last =
        vkr_ui_plot_sample_column(plot, plot->range_last, columns);
    vkr_ui_emit_rect(
        buffer, vkr_ui_plot_column_band(content, first, last, columns, 1.0f),
        plot->range_color, (Vec4){0});
  }
  if (plot->hovered != VKR_UI_PLOT_NONE) {
    const uint32_t column =
        vkr_ui_plot_sample_column(plot, plot->hovered, columns);
    vkr_ui_emit_rect(buffer,
                     vkr_ui_plot_column_band(content, column, column, columns,
                                             Max(1.0f, vkr_round_f32(scale))),
                     plot->hover_color, (Vec4){0});
  }
  if (plot->bar_count) {
    vkr_ui_emit_plot_bars(system, buffer, plot, content, columns);
  }
  if (plot->reference_count) {
    vkr_ui_emit_plot_references(system, buffer, plot, content);
  }
  if (plot->line_count) {
    vkr_ui_emit_plot_lines(system, buffer, plot, content, columns);
  }
  if (plot->cursor != VKR_UI_PLOT_NONE) {
    const uint32_t column =
        vkr_ui_plot_sample_column(plot, plot->cursor, columns);
    VkrUiRect line =
        vkr_ui_plot_column_band(content, column, column, columns, 0.0f);
    const float32_t width = Max(1.0f, vkr_round_f32(1.5f * scale));
    line.x = vkr_round_f32(line.x + (line.width - width) * 0.5f);
    line.width = width;
    vkr_ui_emit_rect(buffer, line, plot->cursor_color, (Vec4){0});
  }
  (void)vkr_ui_draw_buffer_pop_clip(buffer);
}

/* A cubic curve, as one quad per segment. */
vkr_internal void vkr_ui_emit_bezier(const VkrUiSystem *system,
                                     VkrUiDrawBuffer *buffer,
                                     const VkrUiFrameNode *node,
                                     VkrUiRect content) {
  const float32_t scale = system->content_scale;
  Vec2 previous = node->bezier_points[0];
  const float32_t half_width = node->bezier_width * scale * 0.5f;
  const Vec4 color = vkr_ui_linear_color(node->style.text_color);
  const uint32_t segments = vkr_ui_bezier_segments(node);
  for (uint32_t segment = 1; segment <= segments; ++segment) {
    const float32_t t = (float32_t)segment / (float32_t)segments;
    const float32_t u = 1 - t;
    const float32_t weights[4] = {u * u * u, 3 * u * u * t, 3 * u * t * t,
                                  t * t * t};
    Vec2 next = {0};
    for (uint32_t i = 0; i < 4; ++i) {
      next.x += weights[i] * node->bezier_points[i].x;
      next.y += weights[i] * node->bezier_points[i].y;
    }
    const float32_t dx = (next.x - previous.x) * scale;
    const float32_t dy = (next.y - previous.y) * scale;
    const float32_t length = vkr_sqrt_f32(dx * dx + dy * dy);
    if (length > 0.001f) {
      const Vec2 normal = {-dy * half_width / length, dx * half_width / length};
      const Vec2 a = {content.x + previous.x * scale,
                      content.y + previous.y * scale};
      const Vec2 b = {content.x + next.x * scale, content.y + next.y * scale};
      const Vec2 corners[4] = {{a.x + normal.x, a.y + normal.y},
                               {b.x + normal.x, b.y + normal.y},
                               {b.x - normal.x, b.y - normal.y},
                               {a.x - normal.x, a.y - normal.y}};
      (void)vkr_ui_draw_buffer_polygon(buffer, corners, color);
    }
    previous = next;
  }
}

/* The image fitted into the content box over a checkerboard. */
vkr_internal void vkr_ui_emit_image(VkrUiDrawBuffer *buffer,
                                    const VkrUiFrameNode *node,
                                    VkrUiRect content) {
  const float32_t fit = Min(content.width / node->image_size.x,
                            content.height / node->image_size.y);
  VkrUiRect image = {
      content.x + (content.width - node->image_size.x * fit) * 0.5f,
      content.y + (content.height - node->image_size.y * fit) * 0.5f,
      node->image_size.x * fit, node->image_size.y * fit};
  for (uint32_t y = 0; y < 4; ++y) {
    for (uint32_t x = 0; x < 4; ++x) {
      float32_t shade = (x + y) % 2 ? 0.28f : 0.18f;
      VkrUiRect tile = {image.x + image.width * (float32_t)x / 4,
                        image.y + image.height * (float32_t)y / 4,
                        image.width / 4, image.height / 4};
      vkr_ui_emit_rect(buffer, tile, (Vec4){shade, shade, shade, 1}, (Vec4){0});
    }
  }
  (void)vkr_ui_draw_buffer_image(buffer, image, (Vec4){0, 0, 1, 1},
                                 (Vec4){1, 1, 1, 1}, node->image);
}

/* A label's or button's text with its leading and trailing icons. */
vkr_internal void vkr_ui_emit_label(VkrUiSystem *system,
                                    VkrUiDrawBuffer *buffer,
                                    VkrUiFrameNode *node, VkrUiRect content,
                                    const VkrUiTheme *theme) {
  const float32_t scale = system->content_scale;
  /* A trailing icon ends the content box; text and the leading icon lay
     out in what remains. */
  if (node->trailing_icon != VKR_UI_ICON_NONE) {
    const float32_t size =
        Min(node->trailing_icon_size_px, Min(content.width, content.height));
    const VkrUiRect caret = {content.x + content.width - size,
                             content.y + (content.height - size) * 0.5f, size,
                             size};
    Vec4 color = theme->text_secondary;
    color.w *= node->disabled ? 0.45f : 1.0f;
    if (size > 0.0f)
      vkr_ui_emit_icon(system, buffer, node->trailing_icon, caret, color);
    content.width =
        Max(0.0f, content.width - size - VKR_UI_TRAILING_GAP_PT * scale);
  }
  /* A dropdown, a button with a trailing icon, reads from its leading
     edge like a field; other buttons center. */
  const bool8_t centered =
      (node->kind == VKR_UI_NODE_BUTTON && !node->leading &&
       node->trailing_icon == VKR_UI_ICON_NONE) ||
      node->center;
  /* Text never spills past its own box into neighbouring widgets. Wrapped
   * or unconstrained text keeps the parent clip. */
  const float32_t text_width =
      node->retained->text_live
          ? vkr_ui_text_get_bounds(&node->retained->text).size.x
          : 0.0f;
  const float32_t icon_extent =
      node->icon != VKR_UI_ICON_NONE
          ? node->icon_size_px + 6.0f * system->content_scale
          : 0.0f;
  const bool8_t clip_text =
      text_width + icon_extent > content.width + 0.5f && content.width > 0.0f;
  /* Overflowing text may use the padding, as the clip below allows; the
   * ellipsis marks what still does not fit. */
  VkrUiRect text_clip = vkr_ui_uniform_inset(node->rect, 1.0f);
  if (node->trailing_icon != VKR_UI_ICON_NONE)
    text_clip.width = Max(0.0f, content.x + content.width - text_clip.x);
  const float32_t ellipsis_right =
      clip_text ? text_clip.x + text_clip.width : 0.0f;
  if (clip_text && !vkr_ui_draw_buffer_push_clip(buffer, text_clip))
    return;
  if (node->icon == VKR_UI_ICON_NONE) {
    vkr_ui_emit_text(system, buffer, node, content, centered, 0.0f,
                     ellipsis_right);
    if (clip_text)
      (void)vkr_ui_draw_buffer_pop_clip(buffer);
    return;
  }
  const float32_t size =
      Min(node->icon_size_px, Min(content.width, content.height));
  const float32_t gap =
      node->content.length ? 6.0f * system->content_scale : 0.0f;
  const float32_t offset =
      centered ? Max(0.0f, (content.width - size - gap - text_width) * 0.5f)
               : 0.0f;
  const VkrUiRect icon_rect = {content.x + offset,
                               content.y + (content.height - size) * 0.5f, size,
                               size};
  if (size > 0.0f)
    vkr_ui_emit_icon(system, buffer, node->icon, icon_rect, node->icon_color);
  vkr_ui_emit_text(system, buffer, node, content, false_v, offset + size + gap,
                   ellipsis_right);
  if (clip_text)
    (void)vkr_ui_draw_buffer_pop_clip(buffer);
}

vkr_internal void vkr_ui_emit_checkbox(VkrUiSystem *system,
                                       VkrUiDrawBuffer *buffer,
                                       VkrUiFrameNode *node, VkrUiRect content,
                                       const VkrUiTheme *theme) {
  const VkrUiRetainedState *retained = node->retained;
  const float32_t scale = system->content_scale;
  const VkrUiRect box = vkr_ui_checkbox_box(content, scale);
  const float32_t size = box.width;
  const float32_t radius = VKR_UI_CHECKBOX_RADIUS_PT * scale;
  const Vec4 radii = {radius, radius, radius, radius};
  Vec4 fill = node->checked
                  ? vkr_ui_color_mix(theme->accent, theme->accent_hover,
                                     retained->hover_t)
                  : theme->field;
  Vec4 edge = node->checked
                  ? fill
                  : vkr_ui_color_mix(theme->border_strong,
                                     theme->text_secondary, retained->hover_t);
  if (node->disabled) {
    fill.w *= 0.45f;
    edge.w *= 0.45f;
  }
  vkr_ui_emit_box(buffer, box, fill, radii, edge, Max(1.0f, scale));
  if (node->checked) {
    Vec4 mark = theme->text_on_accent;
    mark.w *= node->disabled ? 0.6f : 1.0f;
    vkr_ui_emit_icon(system, buffer, VKR_UI_ICON_CHECK,
                     vkr_ui_uniform_inset(box, size * 0.12f), mark);
  }
  vkr_ui_emit_text(system, buffer, node, content, false_v, size + 7.0f * scale,
                   0.0f);
}

vkr_internal void vkr_ui_emit_slider(const VkrUiSystem *system,
                                     VkrUiDrawBuffer *buffer,
                                     const VkrUiFrameNode *node,
                                     VkrUiRect content,
                                     const VkrUiTheme *theme) {
  const VkrUiRetainedState *retained = node->retained;
  const float32_t scale = system->content_scale;
  const float32_t track_height = Max(2.0f, 4.0f * scale);
  const float32_t knob =
      Min(content.height, (12.0f + 2.0f * retained->hover_t) * scale);
  const float32_t travel = Max(0.0f, content.width - knob);
  const float32_t knob_x = content.x + node->slider_fraction * travel;
  const float32_t track_y = content.y + (content.height - track_height) * 0.5f;
  const Vec4 track_radii = {track_height * 0.5f, track_height * 0.5f,
                            track_height * 0.5f, track_height * 0.5f};
  vkr_ui_emit_rect(buffer,
                   (VkrUiRect){content.x, track_y, content.width, track_height},
                   theme->border, track_radii);
  Vec4 accent =
      vkr_ui_color_mix(theme->accent, theme->accent_hover, retained->hover_t);
  if (node->disabled)
    accent = theme->text_disabled;
  const float32_t filled = knob_x + knob * 0.5f - content.x;
  if (filled > 0.0f)
    vkr_ui_emit_rect(buffer,
                     (VkrUiRect){content.x, track_y, filled, track_height},
                     accent, track_radii);
  const VkrUiRect knob_rect = {
      knob_x, content.y + (content.height - knob) * 0.5f, knob, knob};
  const Vec4 knob_radii = {knob * 0.5f, knob * 0.5f, knob * 0.5f, knob * 0.5f};
  vkr_ui_emit_shadow(buffer, knob_rect, knob_radii, theme->shadow,
                     (Vec2){0.0f, 1.0f * scale}, 3.0f * scale);
  vkr_ui_emit_box(buffer, knob_rect,
                  node->disabled ? theme->text_disabled : theme->text,
                  knob_radii, accent, Max(1.0f, 2.0f * scale));
}

/* A text field's selection, text and caret, clipped to its content box. */
vkr_internal void vkr_ui_emit_text_field(VkrUiSystem *system,
                                         VkrUiDrawBuffer *buffer,
                                         VkrUiFrameNode *node,
                                         VkrUiRect content,
                                         const VkrUiTheme *theme) {
  if (!vkr_ui_draw_buffer_push_clip(buffer, content))
    return;
  VkrUiText *text = &node->retained->text;
  const float32_t height = vkr_ui_text_line_height(text);
  const Vec2 origin = {content.x - node->retained->scroll_offset.x,
                       content.y - node->retained->scroll_offset.y};
  if (system->focused_id == node->id) {
    const uint32_t begin =
        Min(node->retained->text_cursor, node->retained->text_selection);
    const uint32_t end =
        Max(node->retained->text_cursor, node->retained->text_selection);
    uint32_t glyph = 0u;
    for (uint32_t offset = 0u; offset < text->content.length;) {
      const VkrCodepoint cp = vkr_utf8_decode(text->content.str + offset,
                                              text->content.length - offset);
      if (!cp.byte_length)
        break;
      if (cp.value != '\n' && glyph < text->layout.glyphs.length) {
        const VkrTextGlyph *item = &text->layout.glyphs.data[glyph++];
        if (offset >= begin && offset < end)
          vkr_ui_emit_rect(
              buffer,
              (VkrUiRect){origin.x + item->position.x,
                          origin.y + item->position.y - text->bounds.ascent,
                          item->advance, height},
              vkr_ui_color_alpha(theme->selection, 0.9f), (Vec4){0});
      }
      offset += cp.byte_length;
    }
  }
  vkr_ui_emit_text(system, buffer, node, content, false_v, 0.0f, 0.0f);
  if (system->focused_id == node->id && vkr_ui_caret_visible(system)) {
    const Vec2 caret =
        vkr_ui_text_cursor_position(text, node->retained->text_cursor);
    vkr_ui_emit_rect(buffer,
                     (VkrUiRect){origin.x + caret.x, origin.y + caret.y,
                                 Max(1.0f, system->content_scale), height},
                     node->style.text_color, (Vec4){0});
  }
  (void)vkr_ui_draw_buffer_pop_clip(buffer);
}

vkr_internal void vkr_ui_emit_node(VkrUiSystem *system, uint32_t node_index,
                                   VkrUiDrawBuffer *buffer) {
  VkrUiFrameNode *node = &system->frame_nodes[node_index];
  if (!vkr_ui_draw_buffer_push_clip(buffer, node->clip))
    return;
  node->draw_first_command = buffer->command_count;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiRetainedState *retained = node->retained;
  const float32_t scale = system->content_scale;
  Vec4 background = node->style.background_color;
  Vec4 border_color = node->style.border_color;
  vkr_ui_node_state_colors(node, &background, &border_color);
  if (vkr_ui_color_visible(node->style.shadow_color))
    vkr_ui_emit_shadow(buffer, node->rect, node->style.corner_radius_px,
                       node->style.shadow_color, node->style.shadow_offset_px,
                       node->style.shadow_blur_px);
  const VkrUiEdges edges = node->style.border_px;
  const bool8_t uniform_border = edges.top == edges.right &&
                                 edges.top == edges.bottom &&
                                 edges.top == edges.left;
  if (node->style.gradient) {
    /* Authored sRGB endpoints blend in linear space. */
    if (vkr_ui_color_visible(background) ||
        vkr_ui_color_visible(node->style.gradient_color))
      (void)vkr_ui_draw_buffer_gradient(
          buffer, node->rect, vkr_ui_linear_color(background),
          vkr_ui_linear_color(node->style.gradient_color),
          node->style.gradient == VKR_UI_DRAW_GRADIENT_VERTICAL);
  } else if (!vkr_ui_edges_have_extent(edges)) {
    vkr_ui_emit_rect(buffer, node->rect, background,
                     node->style.corner_radius_px);
  } else if (uniform_border) {
    vkr_ui_emit_box(buffer, node->rect, background,
                    node->style.corner_radius_px, border_color, edges.top);
  } else {
    /* Uneven edges, such as a tab's accent top, paint the border color and
     * inset the fill. */
    vkr_ui_emit_rect(buffer, node->rect, border_color,
                     node->style.corner_radius_px);
    vkr_ui_emit_rect(buffer, vkr_ui_rect_inset(node->rect, edges), background,
                     vkr_ui_inner_radii(node->style.corner_radius_px, edges));
  }
  VkrUiRect content = vkr_ui_style_content_rect(node->rect, &node->style);
  switch (node->kind) {
  case VKR_UI_NODE_BEZIER:
    vkr_ui_emit_bezier(system, buffer, node, content);
    break;
  case VKR_UI_NODE_CODE:
    vkr_ui_emit_code(system, buffer, node, content);
    break;
  case VKR_UI_NODE_PLOT:
    vkr_ui_emit_plot(system, buffer, node, content);
    break;
  case VKR_UI_NODE_PATH:
    vkr_ui_emit_path(system, buffer, node, content);
    break;
  case VKR_UI_NODE_IMAGE:
    vkr_ui_emit_image(buffer, node, content);
    break;
  case VKR_UI_NODE_LABEL:
  case VKR_UI_NODE_BUTTON:
    vkr_ui_emit_label(system, buffer, node, content, theme);
    break;
  case VKR_UI_NODE_CHECKBOX:
    vkr_ui_emit_checkbox(system, buffer, node, content, theme);
    break;
  case VKR_UI_NODE_SLIDER:
    vkr_ui_emit_slider(system, buffer, node, content, theme);
    break;
  case VKR_UI_NODE_TEXT_FIELD:
    vkr_ui_emit_text_field(system, buffer, node, content, theme);
    break;
  default:
    break;
  }
  /* Keyboard focus ring follows the widget's radii; a checkbox's rings its
   * box, just outside it. Fields show focus through their border instead. */
  if (node->focusable && node->kind != VKR_UI_NODE_TEXT_FIELD &&
      retained->focus_t > 0.0f && system->focused_id == node->id) {
    const float32_t stroke = Max(1.0f, 1.5f * scale);
    VkrUiRect ring = node->rect;
    Vec4 ring_radii = node->style.corner_radius_px;
    if (node->kind == VKR_UI_NODE_CHECKBOX) {
      const float32_t gap = 2.0f * scale;
      ring = vkr_ui_uniform_inset(vkr_ui_checkbox_box(content, scale), -gap);
      const float32_t radius = VKR_UI_CHECKBOX_RADIUS_PT * scale + gap;
      ring_radii = (Vec4){radius, radius, radius, radius};
    }
    vkr_ui_emit_box(buffer, ring, (Vec4){0}, ring_radii,
                    vkr_ui_color_alpha(theme->focus_ring, retained->focus_t),
                    stroke);
  }
  node->draw_command_count = buffer->command_count - node->draw_first_command;
  for (uint32_t child = node->first_child; child != VKR_UI_NODE_NONE;
       child = system->frame_nodes[child].next_sibling)
    vkr_ui_emit_node(system, child, buffer);
  if (node->kind == VKR_UI_NODE_SCROLL)
    vkr_ui_emit_scrollbar(system, node, buffer);
  node->draw_end_command = buffer->command_count;
  (void)vkr_ui_draw_buffer_pop_clip(buffer);
}

/* Presence transitions (VkrUiPresence): seconds to appear and to fade out.
   Quick enough not to delay a click, long enough to read as motion. */
#define VKR_UI_PRESENCE_IN_SECONDS 0.14f
#define VKR_UI_PRESENCE_OUT_SECONDS 0.10f

vkr_internal VkrUiPresenceState *vkr_ui_presence_find(VkrUiSystem *system,
                                                      VkrUiId id) {
  VkrUiPresenceState *free_state = NULL;
  for (uint32_t i = 0u; i < ArrayCount(system->presences); ++i) {
    VkrUiPresenceState *presence = &system->presences[i];
    if (presence->id == id)
      return presence;
    if (!free_state && presence->id == VKR_UI_ID_NONE)
      free_state = presence;
  }
  if (!free_state)
    return NULL;
  free_state->id = id;
  free_state->t = 0.0f;
  free_state->command_count = 0u;
  free_state->rect_px = (VkrUiRect){0};
  return free_state;
}

/* Advances every transition by this frame's time: panels built this frame
   move toward shown, the others toward hidden, and a hidden one releases its
   slot. Runs once the frame's nodes are known, before its draw hash. */
vkr_internal void vkr_ui_presence_update(VkrUiSystem *system) {
  for (uint32_t i = 0u; i < ArrayCount(system->presences); ++i)
    system->presences[i].shown = false_v;
  for (uint32_t i = 0u; i < system->frame_node_count; ++i) {
    const VkrUiFrameNode *node = &system->frame_nodes[i];
    if (node->presence == VKR_UI_PRESENCE_NONE)
      continue;
    VkrUiPresenceState *presence = vkr_ui_presence_find(system, node->id);
    if (!presence)
      continue;
    presence->kind = node->presence;
    presence->shown = true_v;
  }
  const float32_t dt = (float32_t)system->delta_time;
  for (uint32_t i = 0u; i < ArrayCount(system->presences); ++i) {
    VkrUiPresenceState *presence = &system->presences[i];
    if (presence->id == VKR_UI_ID_NONE)
      continue;
    if (system->reduce_motion)
      presence->t = presence->shown ? 1.0f : 0.0f;
    else if (presence->shown)
      presence->t = Min(1.0f, presence->t + dt / VKR_UI_PRESENCE_IN_SECONDS);
    else
      presence->t = Max(0.0f, presence->t - dt / VKR_UI_PRESENCE_OUT_SECONDS);
    if (!presence->shown && presence->t <= 0.0f) {
      presence->id = VKR_UI_ID_NONE;
      presence->command_count = 0u;
      continue;
    }
    if (presence->t < 1.0f)
      system->animating = true_v;
  }
}

/* Folds the transitions into the frame's draw hash: a moving one redraws. */
vkr_internal uint64_t vkr_ui_presence_hash(const VkrUiSystem *system,
                                           uint64_t hash) {
  for (uint32_t i = 0u; i < ArrayCount(system->presences); ++i) {
    const VkrUiPresenceState *presence = &system->presences[i];
    if (presence->id == VKR_UI_ID_NONE)
      continue;
    hash = vkr_ui_hash_bytes(hash, &presence->id, sizeof(presence->id));
    hash = vkr_ui_hash_bytes(hash, &presence->t, sizeof(presence->t));
    hash = vkr_ui_hash_bytes(hash, &presence->shown, sizeof(presence->shown));
  }
  return hash;
}

vkr_internal Vec2 vkr_ui_presence_point(Vec2 point, Vec2 origin, float32_t s,
                                        float32_t dy) {
  return (Vec2){origin.x + (point.x - origin.x) * s,
                origin.y + (point.y - origin.y) * s + dy};
}

vkr_internal VkrUiRect vkr_ui_presence_rect(VkrUiRect rect, Vec2 origin,
                                            float32_t s, float32_t dy) {
  const Vec2 corner =
      vkr_ui_presence_point((Vec2){rect.x, rect.y}, origin, s, dy);
  return (VkrUiRect){corner.x, corner.y, rect.width * s, rect.height * s};
}

/* Draws `count` commands as the panel at `rect` looks at transition time `t`:
   faded, scaled about its top centre (popup) or centre (window), and a popup
   lifted toward its top edge. Clips move with the content they clip. */
vkr_internal void vkr_ui_presence_transform(VkrUiDrawCommand *commands,
                                            uint32_t count, VkrUiPresence kind,
                                            VkrUiRect rect, float32_t t,
                                            float32_t content_scale) {
  if (t >= 1.0f)
    return;
  const float32_t inverse = 1.0f - t;
  const float32_t eased = 1.0f - inverse * inverse * inverse;
  const bool8_t popup = kind == VKR_UI_PRESENCE_POPUP;
  const float32_t s = popup ? 0.97f + 0.03f * eased : 0.95f + 0.05f * eased;
  const float32_t dy = popup ? -(1.0f - eased) * 6.0f * content_scale : 0.0f;
  const Vec2 origin = {rect.x + rect.width * 0.5f,
                       popup ? rect.y : rect.y + rect.height * 0.5f};
  for (uint32_t i = 0u; i < count; ++i) {
    VkrUiDrawCommand *command = &commands[i];
    command->rect_px = vkr_ui_presence_rect(command->rect_px, origin, s, dy);
    command->clip_rect_px =
        vkr_ui_presence_rect(command->clip_rect_px, origin, s, dy);
    for (uint32_t corner = 0u; corner < command->corner_count; ++corner)
      command->corners_px[corner] =
          vkr_ui_presence_point(command->corners_px[corner], origin, s, dy);
    command->corner_radius_px = vec4_scale(command->corner_radius_px, s);
    command->run_scale *= s;
    command->border_px *= s;
    command->softness_px *= s;
    command->screen_px_range *= s;
    command->color.w *= eased;
    command->border_color.w *= eased;
  }
}

vkr_internal uint64_t vkr_ui_run_bytes(const VkrUiDrawCommand *command) {
  if (command->run == VKR_UI_DRAW_RUN_RECTS) {
    return (uint64_t)command->run_count * sizeof(*command->run_rects);
  }
  if (command->run == VKR_UI_DRAW_RUN_POLYLINE) {
    return (uint64_t)command->run_count * sizeof(*command->run_points);
  }
  return 0u;
}

/* Copies the run data of the presence's kept commands into its own storage
 * and repoints them there, since the frame's data is gone by the time the
 * panel fades out. False without memory; the panel then snaps out. */
vkr_internal bool8_t vkr_ui_presence_keep_runs(VkrUiSystem *system,
                                               VkrUiPresenceState *presence) {
  /* Each copy starts on the alignment of the rectangles' colors. */
  const uint64_t align = AlignOf(VkrUiDrawRect);
  uint64_t bytes = 0u;
  for (uint32_t i = 0u; i < presence->command_count; ++i) {
    bytes += AlignPow2(vkr_ui_run_bytes(&presence->commands[i]), align);
  }
  if (bytes == 0u) {
    return true_v;
  }

  if (bytes > presence->run_data_capacity) {
    uint64_t capacity = Max(presence->run_data_capacity, (uint64_t)KB(4));
    while (capacity < bytes) {
      capacity *= 2u;
    }
    uint8_t *grown = vkr_allocator_alloc(&system->retained_allocator, capacity,
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!grown) {
      return false_v;
    }
    if (presence->run_data) {
      vkr_allocator_free(&system->retained_allocator, presence->run_data,
                         presence->run_data_capacity,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    presence->run_data = grown;
    presence->run_data_capacity = capacity;
  }

  uint8_t *cursor = presence->run_data;
  for (uint32_t i = 0u; i < presence->command_count; ++i) {
    VkrUiDrawCommand *command = &presence->commands[i];
    const uint64_t size = vkr_ui_run_bytes(command);
    if (size == 0u) {
      continue;
    }
    if (command->run == VKR_UI_DRAW_RUN_RECTS) {
      MemCopy(cursor, command->run_rects, size);
      command->run_rects = (const VkrUiDrawRect *)cursor;
    } else {
      MemCopy(cursor, command->run_points, size);
      command->run_points = (const Vec2 *)cursor;
    }
    cursor += AlignPow2(size, align);
  }
  return true_v;
}

/* After the root's commands: keeps each shown transitioning panel's picture
   and draws it at its transition time, then draws the pictures of panels
   fading out above everything but the tooltip. */
vkr_internal void vkr_ui_presence_emit(VkrUiSystem *system,
                                       VkrUiDrawBuffer *buffer) {
  for (uint32_t i = 0u; i < system->frame_node_count; ++i) {
    const VkrUiFrameNode *node = &system->frame_nodes[i];
    if (node->presence == VKR_UI_PRESENCE_NONE ||
        node->draw_end_command <= node->draw_first_command)
      continue;
    VkrUiPresenceState *presence = NULL;
    for (uint32_t j = 0u; j < ArrayCount(system->presences); ++j) {
      if (system->presences[j].id == node->id)
        presence = &system->presences[j];
    }
    if (!presence)
      continue;
    VkrUiDrawCommand *commands = &buffer->commands[node->draw_first_command];
    const uint32_t count = node->draw_end_command - node->draw_first_command;
    if (count > presence->command_capacity) {
      uint32_t capacity = Max(presence->command_capacity, 64u);
      while (capacity < count)
        capacity *= 2u;
      VkrUiDrawCommand *grown = vkr_allocator_alloc(
          &system->retained_allocator, (uint64_t)capacity * sizeof(*grown),
          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      if (grown) {
        if (presence->commands)
          vkr_allocator_free(&system->retained_allocator, presence->commands,
                             (uint64_t)presence->command_capacity *
                                 sizeof(*presence->commands),
                             VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
        presence->commands = grown;
        presence->command_capacity = capacity;
      }
    }
    /* Without room the panel still animates in; it only snaps out. */
    presence->command_count = count <= presence->command_capacity ? count : 0u;
    if (presence->command_count)
      MemCopy(presence->commands, commands, count * sizeof(*commands));
    if (presence->command_count && !vkr_ui_presence_keep_runs(system, presence))
      presence->command_count = 0u;
    presence->rect_px = node->rect;
    vkr_ui_presence_transform(commands, count, presence->kind, node->rect,
                              presence->t, system->content_scale);
  }
  for (uint32_t i = 0u; i < ArrayCount(system->presences); ++i) {
    const VkrUiPresenceState *presence = &system->presences[i];
    if (presence->id == VKR_UI_ID_NONE || presence->shown ||
        presence->command_count == 0u ||
        presence->command_count >
            buffer->command_capacity - buffer->command_count)
      continue;
    VkrUiDrawCommand *commands = &buffer->commands[buffer->command_count];
    MemCopy(commands, presence->commands,
            presence->command_count * sizeof(*commands));
    buffer->command_count += presence->command_count;
    vkr_ui_presence_transform(commands, presence->command_count, presence->kind,
                              presence->rect_px, presence->t,
                              system->content_scale);
  }
}

vkr_internal VkrUiRect vkr_ui_rect_union(VkrUiRect a, VkrUiRect b) {
  if (!vkr_ui_rect_has_area(a))
    return b;
  if (!vkr_ui_rect_has_area(b))
    return a;
  const float32_t right = Max(a.x + a.width, b.x + b.width);
  const float32_t bottom = Max(a.y + a.height, b.y + b.height);
  const float32_t x = Min(a.x, b.x);
  const float32_t y = Min(a.y, b.y);
  return (VkrUiRect){x, y, right - x, bottom - y};
}

vkr_internal VkrUiRect vkr_ui_node_draw_aabb(const VkrUiSystem *system,
                                             const VkrUiFrameNode *node) {
  VkrUiRect aabb = {0};
  for (uint32_t i = 0u; i < node->draw_command_count; ++i) {
    const VkrUiDrawCommand *command =
        &system->frame_commands[node->draw_first_command + i];
    aabb = vkr_ui_rect_union(
        aabb, vkr_ui_tile_command_aabb(command, system->target_width,
                                       system->target_height));
  }
  return aabb;
}

vkr_internal bool8_t vkr_ui_build_tiles(VkrUiSystem *system) {
  VkrUiTileDamage *damage =
      system->retained_count
          ? vkr_allocator_alloc(system->frame_allocator,
                                (uint64_t)system->retained_count *
                                    sizeof(*damage),
                                VKR_ALLOCATOR_MEMORY_TAG_ARRAY)
          : NULL;
  if (system->retained_count && !damage)
    return false_v;
  uint32_t damage_count = 0u;
  for (uint32_t i = 0u; i < system->frame_node_count; ++i) {
    VkrUiFrameNode *node = &system->frame_nodes[i];
    VkrUiRetainedState *retained = node->retained;
    const VkrUiRect current = vkr_ui_node_draw_aabb(system, node);
    // Each frame node owns a distinct retained state (duplicate ids are
    // rejected when nodes are added), so damage never exceeds retained_count.
    assert_log(damage_count < system->retained_count,
               "Frame nodes exceed their retained states");
    damage[damage_count++] = (VkrUiTileDamage){
        .previous_aabb_px = retained->last_draw_aabb,
        .current_aabb_px = current,
    };
    retained->last_draw_aabb = current;
  }
  for (uint32_t i = 0u; i < system->retained_bucket_capacity; ++i) {
    VkrUiRetainedState *retained = system->retained_buckets[i];
    if (!retained || retained == VKR_UI_RETAINED_TOMBSTONE ||
        retained->last_seen_frame == system->frame_index ||
        !vkr_ui_rect_has_area(retained->last_draw_aabb))
      continue;
    assert_log(damage_count < system->retained_count,
               "Unseen retained damage exceeds the retained states");
    damage[damage_count++] = (VkrUiTileDamage){
        .previous_aabb_px = retained->last_draw_aabb,
    };
    retained->last_draw_aabb = (VkrUiRect){0};
  }
  if (!vkr_ui_tile_build(&system->tile_cache, system->frame_allocator,
                         system->target_width, system->target_height,
                         VKR_UI_TILE_SIZE_PX, system->frame_commands,
                         system->frame_command_count, damage, damage_count,
                         &system->tile_frame))
    return false_v;
  system->dirty_tile_ratio = system->tile_frame.dirty_tile_ratio;
  system->dirty_tile_count = system->tile_frame.dirty_tile_count;
  system->tile_count = system->tile_frame.tile_count;
  return true_v;
}

vkr_internal uint32_t vkr_ui_command_estimate(VkrUiSystem *system) {
  uint64_t estimate = 0u;
  for (uint32_t i = 0u; i < system->frame_node_count; ++i) {
    const VkrUiFrameNode *node = &system->frame_nodes[i];
    estimate += node->kind == VKR_UI_NODE_IMAGE ? 26u
                : node->kind == VKR_UI_NODE_BEZIER
                    ? 9u + vkr_ui_bezier_segments(node)
                : node->kind == VKR_UI_NODE_CODE ? 9u + node->code_glyphs
                                                 : 9u;
    /* Range, hover, bars, references and cursor, plus one run per line. */
    if (node->kind == VKR_UI_NODE_PLOT && node->plot)
      estimate += 5u + node->plot->line_count;
    /* At most one stroke per segment. */
    if (node->kind == VKR_UI_NODE_PATH && node->path)
      estimate += node->path->count;
    if (node->icon != VKR_UI_ICON_NONE)
      estimate += 20u;
    if (node->retained->text_live) {
      estimate += node->retained->text.geometry.vertex_count / 4u;
      if (node->kind == VKR_UI_NODE_TEXT_FIELD)
        estimate += node->retained->text.layout.glyphs.length;
    }
  }
  /* Panels fading out redraw their last picture. */
  for (uint32_t i = 0u; i < ArrayCount(system->presences); ++i) {
    const VkrUiPresenceState *presence = &system->presences[i];
    if (presence->id != VKR_UI_ID_NONE && !presence->shown)
      estimate += presence->command_count;
  }
  return (uint32_t)Min(estimate, (uint64_t)VKR_UI_INDEX_CAPACITY / 6u);
}

vkr_internal void vkr_ui_focus_traverse(VkrUiSystem *system) {
  if (!system->keyboard_layer_claimed && system->keyboard_input_layer != 0u) {
    bool8_t has_eligible = false_v;
    for (uint32_t i = 0u; i < system->frame_node_count; ++i)
      has_eligible |= vkr_ui_keyboard_eligible(system, &system->frame_nodes[i]);
    if (!has_eligible)
      system->keyboard_input_layer = 0u;
  }
  for (uint32_t i = 0u; i < system->frame_node_count; ++i) {
    const VkrUiFrameNode *node = &system->frame_nodes[i];
    if (node->id == system->focused_id &&
        !vkr_ui_keyboard_eligible(system, node)) {
      system->focused_id = VKR_UI_ID_NONE;
      break;
    }
  }
  if (!system->keyboard_navigation_enabled ||
      !vkr_ui_key_pressed(system, KEY_TAB) || system->mouse_captured)
    return;
  uint32_t first = VKR_UI_NODE_NONE, last = VKR_UI_NODE_NONE;
  uint32_t previous = VKR_UI_NODE_NONE, next = VKR_UI_NODE_NONE;
  bool8_t passed_focus = false_v;
  for (uint32_t i = 0u; i < system->frame_node_count; ++i) {
    VkrUiFrameNode *node = &system->frame_nodes[i];
    if (!vkr_ui_keyboard_eligible(system, node))
      continue;
    if (first == VKR_UI_NODE_NONE)
      first = i;
    if (node->id == system->focused_id) {
      previous = last;
      passed_focus = true_v;
    } else if (passed_focus && next == VKR_UI_NODE_NONE) {
      next = i;
    }
    last = i;
  }
  const bool8_t reverse = (input_key_press_modifiers(system->input, KEY_TAB) &
                           VKR_INPUT_MOD_SHIFT) != 0;
  const uint32_t target = reverse
                              ? (previous == VKR_UI_NODE_NONE ? last : previous)
                              : (next == VKR_UI_NODE_NONE ? first : next);
  if (target != VKR_UI_NODE_NONE) {
    system->focused_id = system->frame_nodes[target].id;
    system->focused_is_text =
        system->frame_nodes[target].kind == VKR_UI_NODE_TEXT_FIELD;
  }
}

/** One retained tooltip text record, released with the UI system. Its frame
 * node is detached from grid layout and emitted after all ordinary panels. */
vkr_internal uint32_t vkr_ui_tooltip_prepare(VkrUiSystem *system,
                                             uint32_t *out_source) {
  uint32_t source = VKR_UI_NODE_NONE;
  for (uint32_t i = 0u; i < system->frame_node_count; ++i) {
    const VkrUiFrameNode *node = &system->frame_nodes[i];
    if (!node->tooltip.str || node->tooltip.length == 0u)
      continue;
    if (node->id == system->hot_id) {
      source = i;
      break;
    }
    if (system->hot_id == VKR_UI_ID_NONE && node->id == system->focused_id &&
        node->input_layer == system->keyboard_input_layer)
      source = i;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  const float64_t dt = system->delta_time;
  if (source == VKR_UI_NODE_NONE || system->active_id != VKR_UI_ID_NONE) {
    /* Keep the tooltip warm briefly so the next control shows at once. */
    system->tooltip_owner = VKR_UI_ID_NONE;
    system->tooltip_hover_seconds = 0.0;
    system->tooltip_warm_seconds = Max(0.0, system->tooltip_warm_seconds - dt);
    system->tooltip_opacity = 0.0f;
    return VKR_UI_NODE_NONE;
  }
  const VkrUiId owner = system->frame_nodes[source].id;
  if (owner != system->tooltip_owner) {
    system->tooltip_owner = owner;
    system->tooltip_hover_seconds =
        system->tooltip_warm_seconds > 0.0 ? theme->tooltip_delay_seconds : 0.0;
    system->tooltip_opacity =
        system->tooltip_warm_seconds > 0.0 ? system->tooltip_opacity : 0.0f;
  } else {
    system->tooltip_hover_seconds += dt;
  }
  if (!system->reduce_motion &&
      system->tooltip_hover_seconds < theme->tooltip_delay_seconds) {
    system->animating = true_v;
    return VKR_UI_NODE_NONE;
  }
  system->tooltip_warm_seconds = 0.4;
  system->tooltip_opacity =
      vkr_ui_animate(system, system->tooltip_opacity, 1.0f);
  const float32_t opacity = system->tooltip_opacity;
  const String8 text = system->frame_nodes[source].tooltip;
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.style.padding_pt = (VkrUiEdges){5, 8, 5, 8};
  config.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  config.style.corner_radius_pt = (Vec4){5, 5, 5, 5};
  config.style.background_color = vkr_ui_color_alpha(theme->popup, opacity);
  config.style.border_color = vkr_ui_color_alpha(theme->border_strong, opacity);
  config.style.text_color = vkr_ui_color_alpha(theme->text, opacity);
  config.style.shadow_color =
      vkr_ui_color_alpha(theme->shadow, theme->shadow.w * opacity);
  config.style.shadow_offset_pt = (Vec2){0.0f, 2.0f};
  config.style.shadow_blur_pt = 8.0f;
  config.style.font_size_pt = theme->font_caption + 0.5f;
  config.text.layout.word_wrap = true_v;
  config.text.layout.max_width = 360.0f;
  const uint32_t containers = system->container_count;
  system->container_count = 0u;
  const uint32_t index = vkr_ui_add_node(
      system,
      vkr_ui_id_from_label(vkr_ui_id_root(), string8_lit("##ui-tooltip")),
      VKR_UI_NODE_LABEL, config.placement, &config.style);
  system->container_count = containers;
  if (index == VKR_UI_NODE_NONE ||
      !vkr_ui_widget_prepare(system, &system->frame_nodes[index], text,
                             &config))
    return VKR_UI_NODE_NONE;
  *out_source = source;
  return index;
}

VkrUiInputCapture vkr_ui_end(VkrUiSystem *system) {
  if (!system || !system->frame_open)
    return (VkrUiInputCapture){0};
  while (system->container_count > 1u)
    (void)vkr_ui_panel_end(system);
  const VkrUiRetainedState *focused =
      system->focused_id == VKR_UI_ID_NONE
          ? NULL
          : vkr_ui_retained_find(system, system->focused_id);
  if (system->focused_id != VKR_UI_ID_NONE &&
      (!focused || focused->last_seen_frame != system->frame_index))
    system->focused_id = VKR_UI_ID_NONE;
  if (system->mouse_pressed && !system->focus_claimed)
    system->focused_id = VKR_UI_ID_NONE;
  if (system->mouse_released && system->active_id != VKR_UI_ID_NONE)
    system->active_id = VKR_UI_ID_NONE;

  vkr_ui_focus_traverse(system);
  vkr_ui_presence_update(system);
  system->frame_tooltip_source = VKR_UI_NODE_NONE;
  system->frame_tooltip =
      vkr_ui_tooltip_prepare(system, &system->frame_tooltip_source);
  system->frame_draw_pending = true_v;
  system->focused_is_text = false_v;
  for (uint32_t i = 0u; i < system->frame_node_count; ++i) {
    if (system->frame_nodes[i].id == system->focused_id) {
      system->focused_is_text =
          system->frame_nodes[i].kind == VKR_UI_NODE_TEXT_FIELD;
      break;
    }
  }
  system->capture = (VkrUiInputCapture){
      .mouse = system->capture.mouse || system->active_id != VKR_UI_ID_NONE,
      .keyboard =
          system->capture.keyboard || system->focused_id != VKR_UI_ID_NONE,
      .text = system->focused_id != VKR_UI_ID_NONE && system->focused_is_text,
      .hot_id = system->hot_id,
      .active_id = system->active_id,
  };
  system->frame_open = false_v;
  return system->capture;
}

bool8_t vkr_ui_bezier_set_points(VkrUiSystem *system, VkrUiId id,
                                 const Vec2 points[4]) {
  if (!system || (!system->frame_open && !system->frame_draw_pending) ||
      !points || id == VKR_UI_ID_NONE) {
    return false_v;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (!vkr_is_finite_f32(points[i].x) || !vkr_is_finite_f32(points[i].y)) {
      return false_v;
    }
  }
  VkrUiRetainedState *retained = vkr_ui_retained_find(system, id);
  if (!retained || retained->last_seen_frame != system->frame_index) {
    return false_v;
  }
  VkrUiFrameNode *node = &system->frame_nodes[retained->frame_node_index];
  if (node->kind != VKR_UI_NODE_BEZIER) {
    return false_v;
  }
  MemCopy(node->bezier_points, points, sizeof(node->bezier_points));
  return true_v;
}

bool8_t vkr_ui_widget_set_rect(VkrUiSystem *system, VkrUiId id,
                               VkrUiRect rect_pt) {
  if (!system || (!system->frame_open && !system->frame_draw_pending) ||
      id == VKR_UI_ID_NONE || !vkr_ui_rect_is_finite(rect_pt) ||
      !vkr_ui_rect_has_area(rect_pt))
    return false_v;
  VkrUiRetainedState *retained = vkr_ui_retained_find(system, id);
  if (!retained || retained->last_seen_frame != system->frame_index)
    return false_v;
  VkrUiFrameNode *node = &system->frame_nodes[retained->frame_node_index];
  const Vec2 size_px = {rect_pt.width * system->content_scale,
                        rect_pt.height * system->content_scale};
  if (!vkr_is_finite_f32(size_px.x) || !vkr_is_finite_f32(size_px.y))
    return false_v;
  node->placement.margin_pt.left = rect_pt.x;
  node->placement.margin_pt.top = rect_pt.y;
  node->style.min_size_px = size_px;
  node->style.max_size_px = size_px;
  node->intrinsic_size = size_px;
  return true_v;
}

/* Input already reflects the preceding presented geometry. Resolve the next
 * geometry only after scene anchors have received the current camera pose. */
vkr_internal bool8_t vkr_ui_resolve_draw_commands(VkrUiSystem *system) {
  system->frame_draw_pending = false_v;
  const uint32_t tooltip_source = system->frame_tooltip_source;
  const uint32_t tooltip = system->frame_tooltip;
  VkrUiFrameNode *root = &system->frame_nodes[0];
  uint64_t draw_hash = vkr_ui_node_hash(system, 0u);
  if (tooltip != VKR_UI_NODE_NONE) {
    const uint64_t tooltip_hash = vkr_ui_node_hash(system, tooltip);
    draw_hash =
        vkr_ui_hash_bytes(draw_hash, &tooltip_hash, sizeof(tooltip_hash));
    draw_hash = vkr_ui_hash_bytes(
        draw_hash, &system->frame_nodes[tooltip_source].id, sizeof(VkrUiId));
  }
  draw_hash = vkr_ui_presence_hash(system, draw_hash);
  system->frame_draw_hash = draw_hash;
  if (system->draw_cache_valid && system->cached_draw_hash == draw_hash &&
      system->cached_target_width == system->target_width &&
      system->cached_target_height == system->target_height) {
    system->frame_reuses_cached_draw_list = true_v;
    system->frame_draw_ready = true_v;
    system->tile_frame = (VkrUiTileFrame){0};
    system->dirty_tile_ratio = 0.0f;
    system->dirty_tile_count = 0u;
    system->tile_count = system->tile_cache.tile_count;
    vkr_ui_retained_reclaim(system);
    return true_v;
  }
  system->draw_cache_valid = false_v;
  if (!vkr_ui_container_intrinsic(system, root, &root->intrinsic_size)) {
    return vkr_ui_grid_failure(system, root, "root intrinsic");
  }
  const VkrUiRect target = {0.0f, 0.0f, (float32_t)system->target_width,
                            (float32_t)system->target_height};
  if (!vkr_ui_layout_node(system, 0u, target, target)) {
    return vkr_ui_grid_failure(system, root, "root layout");
  }

  if (tooltip != VKR_UI_NODE_NONE) {
    VkrUiFrameNode *node = &system->frame_nodes[tooltip];
    const VkrUiFrameNode *owner = &system->frame_nodes[tooltip_source];
    VkrUiRect anchor = owner->rect;
    /* A plot's tooltip describes the hovered sample, so it starts at that
     * sample's column; the hovered sample is part of the plot's hash. */
    if (owner->kind == VKR_UI_NODE_PLOT && owner->plot &&
        owner->plot->hovered != VKR_UI_PLOT_NONE) {
      const VkrUiRect content =
          vkr_ui_style_content_rect(owner->rect, &owner->style);
      if (vkr_ui_rect_has_area(content)) {
        const uint32_t columns =
            vkr_ui_plot_column_count(system, owner->plot, content.width);
        const uint32_t column = vkr_ui_plot_sample_column(
            owner->plot, owner->plot->hovered, columns);
        anchor.x = vkr_ui_plot_column_x(content, column, columns);
      }
    }
    const float32_t gap = 5.0f * system->content_scale;
    const float32_t width = Min(node->intrinsic_size.x, target.width);
    const float32_t height = Min(node->intrinsic_size.y, target.height);
    float32_t y = anchor.y + anchor.height + gap;
    if (y + height > target.height)
      y = Max(0.0f, anchor.y - height - gap);
    const VkrUiRect rect = {
        vkr_clamp_f32(anchor.x, 0.0f, Max(0.0f, target.width - width)), y,
        width, height};
    (void)vkr_ui_layout_node(system, tooltip, rect, target);
  }
  const uint32_t command_capacity = vkr_ui_command_estimate(system);
  if (command_capacity > 0u) {
    system->frame_commands = vkr_allocator_alloc(
        system->frame_allocator,
        (uint64_t)command_capacity * sizeof(*system->frame_commands),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (system->frame_commands) {
      VkrUiDrawBuffer buffer = {0};
      if (vkr_ui_draw_buffer_begin(&buffer, system->frame_commands,
                                   command_capacity, target)) {
        vkr_ui_emit_node(system, 0u, &buffer);
        vkr_ui_presence_emit(system, &buffer);
        if (tooltip != VKR_UI_NODE_NONE)
          vkr_ui_emit_node(system, tooltip, &buffer);
        system->frame_command_count = buffer.command_count;
        system->frame_command_capacity = command_capacity;
        if (buffer.dropped_command_count &&
            !system->draw_capacity_warning_emitted) {
          log_warn("UI command capacity reached; trailing content was dropped");
          system->draw_capacity_warning_emitted = true_v;
        }
      }
    }
  }
  system->frame_draw_ready =
      command_capacity == 0u || system->frame_commands != NULL;
  if (!vkr_ui_build_tiles(system)) {
    system->dirty_tile_ratio = 1.0f;
    system->dirty_tile_count = 0u;
    system->tile_count = 0u;
    if (!system->tile_build_warning_emitted) {
      log_warn("UI tile binning failed; treating the frame as full damage");
      system->tile_build_warning_emitted = true_v;
    }
  }

  vkr_ui_retained_reclaim(system);
  return system->frame_draw_ready;
}

bool8_t vkr_ui_widget_rect(const VkrUiSystem *system, VkrUiId id,
                           VkrUiRect *out_rect) {
  if (!system || !out_rect || id == VKR_UI_ID_NONE)
    return false_v;
  const VkrUiRetainedState *retained = vkr_ui_retained_find(system, id);
  if (!retained || !vkr_ui_rect_has_area(retained->last_rect))
    return false_v;
  *out_rect = retained->last_rect;
  return true_v;
}

void vkr_ui_text_field_set_cursor(VkrUiSystem *system, VkrUiId id,
                                  uint32_t offset) {
  if (!system || id == VKR_UI_ID_NONE)
    return;
  VkrUiRetainedState *retained = vkr_ui_retained_find(system, id);
  if (!retained)
    return;
  /* The next text_field call clamps both to its buffer and a UTF-8 boundary. */
  retained->text_cursor = offset;
  retained->text_selection = offset;
}

VkrUiInputCapture vkr_ui_system_capture(const VkrUiSystem *system) {
  return system ? system->capture : (VkrUiInputCapture){0};
}

float32_t vkr_ui_system_dirty_tile_ratio(const VkrUiSystem *system) {
  return system ? system->dirty_tile_ratio : 1.0f;
}

bool8_t vkr_ui_system_prepare_draw_list(VkrUiSystem *system,
                                        VkrAllocator *frame_allocator,
                                        uint32_t target_width,
                                        uint32_t target_height,
                                        VkrPreparedUiDrawList *out_draw_list) {
  if (!system || !frame_allocator || !out_draw_list || target_width == 0u ||
      target_height == 0u)
    return false_v;
  *out_draw_list = (VkrPreparedUiDrawList){0};
  if (system->frame_open || target_width != system->target_width ||
      target_height != system->target_height)
    return false_v;
  if (system->frame_draw_pending && !vkr_ui_resolve_draw_commands(system))
    return false_v;
  if (!system->frame_draw_ready)
    return false_v;
  if (system->draw_cache_valid &&
      system->cached_draw_hash == system->frame_draw_hash &&
      system->cached_target_width == target_width &&
      system->cached_target_height == target_height) {
    *out_draw_list = (VkrPreparedUiDrawList){
        .vertices = system->cached_vertices,
        .vertex_count = system->cached_vertex_count,
        .indices = system->cached_indices,
        .index_count = system->cached_index_count,
        .batches = system->cached_batches,
        .batch_count = system->cached_batch_count,
    };
    return true_v;
  }

  if (!system->frame_commands || system->frame_command_count == 0u) {
    system->cached_vertex_count = 0u;
    system->cached_index_count = 0u;
    system->cached_batch_count = 0u;
    system->cached_draw_hash = system->frame_draw_hash;
    system->cached_target_width = target_width;
    system->cached_target_height = target_height;
    system->draw_cache_valid = true_v;
    return true_v;
  }

  VkrUiDrawCommand *commands = system->frame_commands;
  const uint32_t command_count = system->frame_command_count;
  const uint32_t command_capacity = system->frame_command_capacity;
  system->frame_commands = NULL;
  system->frame_command_count = 0u;
  system->frame_command_capacity = 0u;
  (void)frame_allocator;
  const VkrUiDrawBuffer buffer = {
      .commands = commands,
      .command_count = command_count,
      .command_capacity = command_capacity,
      .clip_stack = {{0.0f, 0.0f, (float32_t)target_width,
                      (float32_t)target_height}},
      .clip_count = 1u,
  };
  VkrUiDrawOutput output = {
      .vertices = system->cached_vertices,
      .vertex_capacity = VKR_UI_VERTEX_CAPACITY,
      .indices = system->cached_indices,
      .index_capacity = VKR_UI_INDEX_CAPACITY,
      .batches = system->cached_batches,
      .batch_capacity = VKR_UI_BATCH_CAPACITY,
  };
  const VkrUiDrawBuildResult result =
      vkr_ui_draw_build(&buffer, target_width, target_height, &output);
  if (result.status == VKR_UI_DRAW_BUILD_INVALID)
    return false_v;
  if (result.status == VKR_UI_DRAW_BUILD_TRUNCATED &&
      !system->draw_capacity_warning_emitted) {
    log_warn("UI geometry capacity reached; trailing content was dropped");
    system->draw_capacity_warning_emitted = true_v;
  }
  system->cached_vertex_count = output.vertex_count;
  system->cached_index_count = output.index_count;
  system->cached_batch_count = output.batch_count;
  system->cached_draw_hash = system->frame_draw_hash;
  system->cached_target_width = target_width;
  system->cached_target_height = target_height;
  system->draw_cache_valid = true_v;
  *out_draw_list = (VkrPreparedUiDrawList){
      .vertices = system->cached_vertices,
      .vertex_count = system->cached_vertex_count,
      .indices = system->cached_indices,
      .index_count = system->cached_index_count,
      .batches = system->cached_batches,
      .batch_count = system->cached_batch_count,
  };
  return true_v;
}
