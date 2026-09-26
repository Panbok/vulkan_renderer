#include "renderer/systems/vkr_ui_system.h"

#include "core/logger.h"
#include "core/vkr_window.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_font_system.h"
#include "vkr_color_transfer.h"
#include "vkr_frame_input.h"

#include <float.h>
#include <math.h>

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
} VkrUiNodeKind;

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
  /* Scroll areas: the thumb is held, grabbed this far below its top edge. */
  bool8_t scroll_dragging;
  float32_t scroll_grab_offset;
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
  VkrUiTextureRef image;
  Vec2 image_size;
  Vec2 bezier_points[4];
  float32_t bezier_width;
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
  float32_t slider_fraction;
  bool8_t checked;
  bool8_t hovered;
  bool8_t active;
  bool8_t clip_children;
};

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
  if (!system || !isfinite(scale))
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
  if (!system || !isfinite(content_scale) || content_scale <= 0.0f ||
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
        (isfinite(config->icon_size_pt) && config->icon_size_pt > 0.0f
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
  const float32_t step =
      1.0f - expf(-vkr_ui_theme()->motion_rate * (float32_t)system->delta_time);
  value += (target - value) * vkr_clamp_f32(step, 0.0f, 1.0f);
  if (fabsf(target - value) < 0.004f)
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
      system->frame_open || !isfinite(delta_time) || delta_time < 0.0)
    return false_v;
  uint32_t width = 0u;
  uint32_t height = 0u;
  if (!vkr_ui_system_dimensions(window, system, target_width, target_height,
                                &width, &height))
    return false_v;
  VkrWindowContentScale scale = vkr_ui_system_content_scale(window, system);
  if (!isfinite(scale.value) || scale.value <= 0.0f)
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
  if (vkr_ui_point_in_rect(system->mouse_x, system->mouse_y, rect_px) &&
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
      !isfinite(source_size.x) || !isfinite(source_size.y) ||
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
  if (!system || !system->frame_open || !points || !isfinite(width_pt) ||
      width_pt <= 0) {
    return;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (!isfinite(points[i].x) || !isfinite(points[i].y)) {
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
  if (!system || !system->frame_open || !value || !isfinite(*value) ||
      !isfinite(minimum) || !isfinite(maximum) || maximum <= minimum)
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
 * gutter consumes that press so rows beneath it do not also activate. */
vkr_internal void vkr_ui_scroll_drag(VkrUiSystem *system,
                                     VkrUiFrameNode *node) {
  VkrUiRetainedState *retained = node->retained;
  VkrUiRect gutter;
  VkrUiRect thumb;
  if (!vkr_ui_scroll_thumb(retained, &node->style, system->content_scale,
                           &gutter, &thumb)) {
    retained->scroll_dragging = false_v;
    return;
  }
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
    }
    system->mouse_pressed = false_v;
    system->capture.mouse = true_v;
  }
  if (!retained->scroll_dragging)
    return;
  if (system->mouse_released ||
      !input_is_button_down(system->input, BUTTON_LEFT)) {
    retained->scroll_dragging = false_v;
    return;
  }
  const float32_t travel = Max(1.0f, viewport - thumb.height);
  const float32_t top =
      (float32_t)system->mouse_y - retained->scroll_grab_offset - gutter.y;
  retained->scroll_offset.y = vkr_clamp_f32(top / travel, 0.0f, 1.0f) * range;
  system->capture.mouse = true_v;
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
  vkr_ui_scroll_drag(system, node);
  if (node->hovered && system->mouse_wheel != 0)
    node->retained->scroll_offset.y =
        Max(0.0f,
            node->retained->scroll_offset.y -
                (float32_t)system->mouse_wheel * 32.0f * system->content_scale);
  if (vkr_ui_keyboard_eligible(system, node) && system->focused_id == id) {
    const float32_t page =
        vkr_ui_style_content_rect(node->retained->last_rect, &node->style)
            .height;
    if (vkr_ui_key_pressed(system, KEY_HOME))
      node->retained->scroll_offset.y = 0.0f;
    else if (vkr_ui_key_pressed(system, KEY_END))
      // Layout clamps this request to the current declared row extent.
      node->retained->scroll_offset.y = FLT_MAX;
    else {
      const int32_t direction = (int32_t)vkr_ui_key_pressed(system, KEY_NEXT) -
                                (int32_t)vkr_ui_key_pressed(system, KEY_PRIOR);
      node->retained->scroll_offset.y =
          Max(0.0f, node->retained->scroll_offset.y + direction * page);
    }
  }
  system->container_stack[system->container_count++] = index;
  return vkr_ui_id_stack_push_label(&system->id_stack, id_label);
}

bool8_t vkr_ui_scroll_area_offset_set(VkrUiSystem *system,
                                      float32_t offset_pt) {
  if (!system || !system->frame_open || !system->container_count ||
      !isfinite(offset_pt) || offset_pt < 0)
    return false_v;
  VkrUiFrameNode *node =
      &system
           ->frame_nodes[system->container_stack[system->container_count - 1u]];
  if (node->kind != VKR_UI_NODE_SCROLL)
    return false_v;
  node->retained->scroll_offset.y = offset_pt * system->content_scale;
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
  if (codepoint < 0x20u || codepoint == 0x7fu)
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
  float32_t distance = INFINITY;
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
    const float32_t cost = dy * 10000.0f + fabsf(mouse.x - caret.x);
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
  const uint32_t previous_cursor = retained->text_cursor;
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
        if (!vkr_ui_shift_down(system))
          retained->text_selection = retained->text_cursor;
        retained->text_dragging = true_v;
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
  if (vkr_ui_keyboard_eligible(system, node) && system->focused_id == id) {
    system->focused_is_text = true_v;
    uint32_t selection_begin =
        Min(retained->text_cursor, retained->text_selection);
    uint32_t selection_end =
        Max(retained->text_cursor, retained->text_selection);
    const bool8_t shortcut =
        input_key_shortcut_modifier(system->input, KEY_A) ||
        input_key_shortcut_modifier(system->input, KEY_C) ||
        input_key_shortcut_modifier(system->input, KEY_X) ||
        input_key_shortcut_modifier(system->input, KEY_V);
    if (input_key_shortcut_modifier(system->input, KEY_A) &&
        vkr_ui_key_pressed(system, KEY_A)) {
      retained->text_selection = 0u;
      retained->text_cursor = buffer->length;
      retained->text_dragging = false_v;
    } else if ((input_key_shortcut_modifier(system->input, KEY_C) &&
                vkr_ui_key_pressed(system, KEY_C)) ||
               (input_key_shortcut_modifier(system->input, KEY_X) &&
                vkr_ui_key_pressed(system, KEY_X))) {
      if (selection_end > selection_begin &&
          vkr_platform_clipboard_write_text(buffer->data + selection_begin,
                                            selection_end - selection_begin) &&
          !config->read_only &&
          input_key_shortcut_modifier(system->input, KEY_X) &&
          vkr_ui_key_pressed(system, KEY_X)) {
        vkr_ui_text_edit_erase(buffer, selection_begin, selection_end);
        retained->text_cursor = selection_begin;
        retained->text_selection = selection_begin;
        changed = true_v;
      }
    } else if (!config->read_only &&
               input_key_shortcut_modifier(system->input, KEY_V) &&
               vkr_ui_key_pressed(system, KEY_V)) {
      // This bounded paste copy expires with the caller's frame scratch.
      uint8_t *paste =
          vkr_allocator_alloc(system->frame_allocator, buffer->capacity,
                              VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      uint32_t length = 0u;
      if (paste &&
          vkr_platform_clipboard_read_text(paste, buffer->capacity, &length)) {
        for (uint32_t offset = 0u; offset < length;) {
          const VkrCodepoint cp =
              vkr_utf8_decode(paste + offset, length - offset);
          if (cp.byte_length == 0u)
            break;
          if (cp.value >= 0x20u && cp.value != 0x7fu) {
            if (!vkr_ui_text_edit_insert(buffer, &retained->text_cursor,
                                         &retained->text_selection, cp.value))
              break;
            changed = true_v;
          }
          offset += cp.byte_length;
        }
      }
    }
    if (!config->read_only && !shortcut) {
      uint32_t character_count = 0u;
      const uint32_t *characters =
          input_get_characters(system->input, &character_count);
      for (uint32_t i = 0u; i < character_count; ++i)
        changed |=
            vkr_ui_text_edit_insert(buffer, &retained->text_cursor,
                                    &retained->text_selection, characters[i]);
    }
    selection_begin = Min(retained->text_cursor, retained->text_selection);
    selection_end = Max(retained->text_cursor, retained->text_selection);
    if (!config->read_only && vkr_ui_key_repeat(system, KEY_BACKSPACE)) {
      if (selection_begin == selection_end)
        selection_begin = vkr_ui_utf8_previous(buffer->data, selection_begin);
      const uint32_t old_length = buffer->length;
      vkr_ui_text_edit_erase(buffer, selection_begin, selection_end);
      retained->text_cursor = selection_begin;
      retained->text_selection = selection_begin;
      changed |= buffer->length != old_length;
    } else if (!config->read_only && vkr_ui_key_repeat(system, KEY_DELETE)) {
      if (selection_begin == selection_end)
        selection_end =
            vkr_ui_utf8_next(buffer->data, buffer->length, selection_end);
      const uint32_t old_length = buffer->length;
      vkr_ui_text_edit_erase(buffer, selection_begin, selection_end);
      retained->text_cursor = selection_begin;
      retained->text_selection = selection_begin;
      changed |= buffer->length != old_length;
    }
    bool8_t shift = vkr_ui_shift_down(system);
    bool8_t moved = false_v;
    if (vkr_ui_key_repeat(system, KEY_LEFT)) {
      shift = (input_key_press_modifiers(system->input, KEY_LEFT) &
               VKR_INPUT_MOD_SHIFT) != 0;
      retained->text_cursor =
          !shift && selection_begin != selection_end
              ? selection_begin
              : vkr_ui_utf8_previous(buffer->data, retained->text_cursor);
      moved = true_v;
    }
    if (vkr_ui_key_repeat(system, KEY_RIGHT)) {
      shift = (input_key_press_modifiers(system->input, KEY_RIGHT) &
               VKR_INPUT_MOD_SHIFT) != 0;
      retained->text_cursor =
          !shift && selection_begin != selection_end
              ? selection_end
              : vkr_ui_utf8_next(buffer->data, buffer->length,
                                 retained->text_cursor);
      moved = true_v;
    }
    const int32_t vertical = (int32_t)vkr_ui_key_repeat(system, KEY_DOWN) -
                             (int32_t)vkr_ui_key_repeat(system, KEY_UP);
    if (vertical && retained->text.layout.line_count > 1u) {
      shift = (input_key_press_modifiers(system->input,
                                         vertical > 0 ? KEY_DOWN : KEY_UP) &
               VKR_INPUT_MOD_SHIFT) != 0;
      Vec2 target =
          vkr_ui_text_cursor_position(&retained->text, retained->text_cursor);
      target.y += ((float32_t)vertical + 0.5f) *
                  vkr_ui_text_line_height(&retained->text);
      retained->text_cursor = vkr_ui_text_mouse_cursor(&retained->text, target);
      moved = true_v;
    }
    if (vkr_ui_key_pressed(system, KEY_HOME)) {
      shift = (input_key_press_modifiers(system->input, KEY_HOME) &
               VKR_INPUT_MOD_SHIFT) != 0;
      retained->text_cursor = 0u;
      moved = true_v;
    }
    if (vkr_ui_key_pressed(system, KEY_END)) {
      shift = (input_key_press_modifiers(system->input, KEY_END) &
               VKR_INPUT_MOD_SHIFT) != 0;
      retained->text_cursor = buffer->length;
      moved = true_v;
    }
    if (moved && !shift)
      retained->text_selection = retained->text_cursor;
    // Typing or keyboard navigation takes ownership of the caret. A later
    // mouse-up must not reselect text at the old pointer position.
    if (changed || moved)
      retained->text_dragging = false_v;
  }
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
  return system->reduce_motion || fmod(system->time_seconds, 1.06) < 0.62;
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
  hash = vkr_ui_hash_bytes(hash, &node->disabled, sizeof(node->disabled));
  hash = vkr_ui_hash_bytes(hash, &node->fill, sizeof(node->fill));
  hash = vkr_ui_hash_bytes(hash, &node->center, sizeof(node->center));
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
  return (VkrUiGridItem){
      .column = placement.column,
      .row = placement.row,
      .column_span = Min(placement.column_span, columns),
      .row_span = Min(placement.row_span, rows),
      .justify = text_widget && placement.justify == VKR_UI_ALIGN_STRETCH
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
    top = roundf(top + bounds.ascent) - bounds.ascent;
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
  float32_t cut = FLT_MAX;
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
  if (!vkr_ui_edges_have_extent(edges)) {
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
  const VkrUiRect content = vkr_ui_style_content_rect(node->rect, &node->style);
  switch (node->kind) {
  case VKR_UI_NODE_BEZIER: {
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
      const float32_t length = sqrtf(dx * dx + dy * dy);
      if (length > 0.001f) {
        const Vec2 normal = {-dy * half_width / length,
                             dx * half_width / length};
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
    break;
  }
  case VKR_UI_NODE_IMAGE: {
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
        vkr_ui_emit_rect(buffer, tile, (Vec4){shade, shade, shade, 1},
                         (Vec4){0});
      }
    }
    (void)vkr_ui_draw_buffer_image(buffer, image, (Vec4){0, 0, 1, 1},
                                   (Vec4){1, 1, 1, 1}, node->image);
    break;
  }
  case VKR_UI_NODE_LABEL:
  case VKR_UI_NODE_BUTTON: {
    const bool8_t centered = node->kind == VKR_UI_NODE_BUTTON || node->center;
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
    const float32_t ellipsis_right =
        clip_text ? node->rect.x + node->rect.width - 1.0f : 0.0f;
    if (clip_text && !vkr_ui_draw_buffer_push_clip(
                         buffer, vkr_ui_uniform_inset(node->rect, 1.0f)))
      break;
    if (node->icon == VKR_UI_ICON_NONE) {
      vkr_ui_emit_text(system, buffer, node, content, centered, 0.0f,
                       ellipsis_right);
      if (clip_text)
        (void)vkr_ui_draw_buffer_pop_clip(buffer);
      break;
    }
    const float32_t size =
        Min(node->icon_size_px, Min(content.width, content.height));
    const float32_t gap =
        node->content.length ? 6.0f * system->content_scale : 0.0f;
    const float32_t offset =
        centered ? Max(0.0f, (content.width - size - gap - text_width) * 0.5f)
                 : 0.0f;
    const VkrUiRect icon_rect = {content.x + offset,
                                 content.y + (content.height - size) * 0.5f,
                                 size, size};
    if (size > 0.0f)
      vkr_ui_emit_icon(system, buffer, node->icon, icon_rect, node->icon_color);
    vkr_ui_emit_text(system, buffer, node, content, false_v,
                     offset + size + gap, ellipsis_right);
    if (clip_text)
      (void)vkr_ui_draw_buffer_pop_clip(buffer);
    break;
  }
  case VKR_UI_NODE_CHECKBOX: {
    const float32_t size = Min(content.height, 15.0f * scale);
    const VkrUiRect box = {
        content.x, content.y + (content.height - size) * 0.5f, size, size};
    const float32_t radius = 3.0f * scale;
    const Vec4 radii = {radius, radius, radius, radius};
    Vec4 fill = node->checked
                    ? vkr_ui_color_mix(theme->accent, theme->accent_hover,
                                       retained->hover_t)
                    : theme->field;
    Vec4 edge = node->checked ? fill
                              : vkr_ui_color_mix(theme->border_strong,
                                                 theme->text_secondary,
                                                 retained->hover_t);
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
    vkr_ui_emit_text(system, buffer, node, content, false_v,
                     size + 7.0f * scale, 0.0f);
    break;
  }
  case VKR_UI_NODE_SLIDER: {
    const float32_t track_height = Max(2.0f, 4.0f * scale);
    const float32_t knob =
        Min(content.height, (12.0f + 2.0f * retained->hover_t) * scale);
    const float32_t travel = Max(0.0f, content.width - knob);
    const float32_t knob_x = content.x + node->slider_fraction * travel;
    const float32_t track_y =
        content.y + (content.height - track_height) * 0.5f;
    const Vec4 track_radii = {track_height * 0.5f, track_height * 0.5f,
                              track_height * 0.5f, track_height * 0.5f};
    vkr_ui_emit_rect(
        buffer, (VkrUiRect){content.x, track_y, content.width, track_height},
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
    const Vec4 knob_radii = {knob * 0.5f, knob * 0.5f, knob * 0.5f,
                             knob * 0.5f};
    vkr_ui_emit_shadow(buffer, knob_rect, knob_radii, theme->shadow,
                       (Vec2){0.0f, 1.0f * scale}, 3.0f * scale);
    vkr_ui_emit_box(buffer, knob_rect,
                    node->disabled ? theme->text_disabled : theme->text,
                    knob_radii, accent, Max(1.0f, 2.0f * scale));
    break;
  }
  case VKR_UI_NODE_TEXT_FIELD: {
    if (!vkr_ui_draw_buffer_push_clip(buffer, content))
      break;
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
    break;
  }
  default:
    break;
  }
  /* Keyboard focus ring follows the widget's radii. Fields show focus through
   * their border instead. */
  if (node->focusable && node->kind != VKR_UI_NODE_TEXT_FIELD &&
      retained->focus_t > 0.0f && system->focused_id == node->id) {
    const float32_t stroke = Max(1.0f, 1.5f * scale);
    vkr_ui_emit_box(buffer, node->rect, (Vec4){0}, node->style.corner_radius_px,
                    vkr_ui_color_alpha(theme->focus_ring, retained->focus_t),
                    stroke);
  }
  node->draw_command_count = buffer->command_count - node->draw_first_command;
  for (uint32_t child = node->first_child; child != VKR_UI_NODE_NONE;
       child = system->frame_nodes[child].next_sibling)
    vkr_ui_emit_node(system, child, buffer);
  if (node->kind == VKR_UI_NODE_SCROLL)
    vkr_ui_emit_scrollbar(system, node, buffer);
  (void)vkr_ui_draw_buffer_pop_clip(buffer);
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
                    : 9u;
    if (node->icon != VKR_UI_ICON_NONE)
      estimate += 20u;
    if (node->retained->text_live) {
      estimate += node->retained->text.geometry.vertex_count / 4u;
      if (node->kind == VKR_UI_NODE_TEXT_FIELD)
        estimate += node->retained->text.layout.glyphs.length;
    }
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
    if (!isfinite(points[i].x) || !isfinite(points[i].y)) {
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
  if (!isfinite(size_px.x) || !isfinite(size_px.y))
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
    const VkrUiRect anchor = system->frame_nodes[tooltip_source].rect;
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
