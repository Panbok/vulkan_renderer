#include "renderer/systems/vkr_ui_system.h"

#include "core/logger.h"
#include "core/vkr_window.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_font_system.h"
#include "renderer/vkr_color_transfer.h"
#include "renderer/vkr_frame_input.h"

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
} VkrUiNodeKind;

struct VkrUiRetainedState {
  VkrUiId id;
  VkrUiNodeKind kind;
  uint64_t last_seen_frame;
  uint64_t build_hash;
  VkrUiRect last_rect;
  VkrUiRect last_clip;
  VkrUiRect last_draw_aabb;
  VkrUiText text;
  Vec2 scroll_offset;
  uint32_t text_cursor;
  uint32_t text_selection;
  float32_t animation_phase;
  bool8_t text_live;
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
  if (window)
    return vkr_window_get_content_scale(window);
  return (VkrWindowContentScale){.value = 1.0f, .revision = 1u};
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
  node->content = content;
  const VkrTextBounds bounds = vkr_ui_text_get_bounds(&retained->text);
  node->intrinsic_size = (Vec2){
      bounds.size.x + node->style.padding_px.left +
          node->style.padding_px.right + node->style.border_px.left +
          node->style.border_px.right,
      bounds.size.y + node->style.padding_px.top +
          node->style.padding_px.bottom + node->style.border_px.top +
          node->style.border_px.bottom,
  };
  node->intrinsic_size =
      vkr_ui_style_clamp_size(node->intrinsic_size, &node->style);
  return true_v;
}

vkr_internal bool8_t vkr_ui_interact(VkrUiSystem *system, VkrUiFrameNode *node,
                                     bool8_t focusable) {
  VkrUiRetainedState *retained = node->retained;
  const bool8_t hovered = !system->mouse_captured &&
                          system->input_layer == system->mouse_input_layer &&
                          vkr_ui_rect_has_area(retained->last_rect) &&
                          vkr_ui_point_in_rect(system->mouse_x, system->mouse_y,
                                               retained->last_rect);
  node->hovered = hovered;
  if (hovered) {
    system->hot_id = node->id;
    system->capture.mouse = true_v;
  }
  if (system->mouse_pressed && hovered && system->active_id == VKR_UI_ID_NONE) {
    system->active_id = node->id;
    if (focusable) {
      system->focused_id = node->id;
      system->focus_claimed = true_v;
    }
  }
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
  system->mouse_pressed = input_is_button_down(input, BUTTON_LEFT) &&
                          input_was_button_up(input, BUTTON_LEFT);
  system->mouse_released = input_is_button_up(input, BUTTON_LEFT) &&
                           input_was_button_down(input, BUTTON_LEFT);
  system->focus_claimed = false_v;
  system->focused_is_text = false_v;
  system->hot_id = VKR_UI_ID_NONE;
  system->input_layer = 0u;
  system->mouse_input_layer = 0u;
  system->capture = (VkrUiInputCapture){0};
  system->frame_commands = NULL;
  system->frame_command_count = 0u;
  system->frame_command_capacity = 0u;
  system->frame_draw_hash = 0u;
  system->frame_draw_ready = false_v;
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
    (void)vkr_ui_text_prepare(system, &system->frame_nodes[index], content,
                              &config->text);
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
      !vkr_ui_text_prepare(system, &system->frame_nodes[index], content,
                           &config->text))
    return false_v;
  return vkr_ui_interact(system, &system->frame_nodes[index], true_v);
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
      !vkr_ui_text_prepare(system, &system->frame_nodes[index], content,
                           &config->text))
    return false_v;
  VkrUiFrameNode *node = &system->frame_nodes[index];
  const float32_t box = 16.0f * system->content_scale;
  node->intrinsic_size.x += box + 6.0f * system->content_scale;
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
  (void)vkr_ui_interact(system, node, true_v);
  bool8_t changed = false_v;
  if (system->active_id == id &&
      input_is_button_down(system->input, BUTTON_LEFT) &&
      node->retained->last_rect.width > 0.0f) {
    const float32_t fraction = vkr_clamp_f32(
        ((float32_t)system->mouse_x - node->retained->last_rect.x) /
            node->retained->last_rect.width,
        0.0f, 1.0f);
    const float32_t next = minimum + fraction * (maximum - minimum);
    changed = next != *value;
    *value = next;
  }
  node->slider_fraction =
      vkr_clamp_f32((*value - minimum) / (maximum - minimum), 0.0f, 1.0f);
  return changed;
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
  (void)vkr_ui_interact(system, node, false_v);
  if (node->hovered && system->mouse_wheel != 0)
    node->retained->scroll_offset.y =
        Max(0.0f,
            node->retained->scroll_offset.y -
                (float32_t)system->mouse_wheel * 32.0f * system->content_scale);
  system->container_stack[system->container_count++] = index;
  return vkr_ui_id_stack_push_label(&system->id_stack, id_label);
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
  if (input_is_key_up(system->input, key))
    return false_v;
  if (input_key_just_pressed(system->input, key)) {
    system->repeat_key = key;
    system->repeat_elapsed = 0.0;
    system->repeat_next = VKR_UI_KEY_REPEAT_DELAY_SECONDS;
    return true_v;
  }
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

bool8_t vkr_ui_text_field(VkrUiSystem *system, String8 id_label,
                          VkrUiTextEditBuffer *buffer,
                          const VkrUiWidgetConfig *source_config) {
  if (!system || !system->frame_open || !buffer || !buffer->data ||
      buffer->capacity == 0u || buffer->length >= buffer->capacity)
    return false_v;
  buffer->data[buffer->length] = 0u;
  VkrUiWidgetConfig fallback = vkr_ui_widget_config_default();
  fallback.style.padding_pt = (VkrUiEdges){4.0f, 6.0f, 4.0f, 6.0f};
  fallback.style.min_size_pt = (Vec2){120.0f, 24.0f};
  fallback.style.background_color = (Vec4){0.08f, 0.09f, 0.11f, 1.0f};
  const VkrUiWidgetConfig *config = source_config ? source_config : &fallback;
  const VkrUiId id = vkr_ui_id_stack_widget_label(&system->id_stack, id_label);
  const uint32_t index = vkr_ui_add_node(system, id, VKR_UI_NODE_TEXT_FIELD,
                                         config->placement, &config->style);
  if (index == VKR_UI_NODE_NONE)
    return false_v;
  VkrUiFrameNode *node = &system->frame_nodes[index];
  (void)vkr_ui_interact(system, node, true_v);
  VkrUiRetainedState *retained = node->retained;
  retained->text_cursor = Min(retained->text_cursor, buffer->length);
  retained->text_selection = Min(retained->text_selection, buffer->length);
  bool8_t changed = false_v;
  if (system->focused_id == id) {
    system->focus_claimed = true_v;
    system->focused_is_text = true_v;
    uint32_t selection_begin =
        Min(retained->text_cursor, retained->text_selection);
    uint32_t selection_end =
        Max(retained->text_cursor, retained->text_selection);
    uint32_t character_count = 0u;
    const uint32_t *characters =
        input_get_characters(system->input, &character_count);
    for (uint32_t i = 0u; i < character_count; ++i)
      changed |=
          vkr_ui_text_edit_insert(buffer, &retained->text_cursor,
                                  &retained->text_selection, characters[i]);
    selection_begin = Min(retained->text_cursor, retained->text_selection);
    selection_end = Max(retained->text_cursor, retained->text_selection);
    if (vkr_ui_key_repeat(system, KEY_BACKSPACE)) {
      if (selection_begin == selection_end)
        selection_begin = vkr_ui_utf8_previous(buffer->data, selection_begin);
      const uint32_t old_length = buffer->length;
      vkr_ui_text_edit_erase(buffer, selection_begin, selection_end);
      retained->text_cursor = selection_begin;
      retained->text_selection = selection_begin;
      changed |= buffer->length != old_length;
    } else if (vkr_ui_key_repeat(system, KEY_DELETE)) {
      if (selection_begin == selection_end)
        selection_end =
            vkr_ui_utf8_next(buffer->data, buffer->length, selection_end);
      const uint32_t old_length = buffer->length;
      vkr_ui_text_edit_erase(buffer, selection_begin, selection_end);
      retained->text_cursor = selection_begin;
      retained->text_selection = selection_begin;
      changed |= buffer->length != old_length;
    }
    if (vkr_ui_key_repeat(system, KEY_LEFT))
      retained->text_cursor =
          vkr_ui_utf8_previous(buffer->data, retained->text_cursor);
    if (vkr_ui_key_repeat(system, KEY_RIGHT))
      retained->text_cursor =
          vkr_ui_utf8_next(buffer->data, buffer->length, retained->text_cursor);
    if (input_is_key_down(system->input, KEY_HOME) &&
        input_was_key_up(system->input, KEY_HOME))
      retained->text_cursor = 0u;
    if (input_is_key_down(system->input, KEY_END) &&
        input_was_key_up(system->input, KEY_END))
      retained->text_cursor = buffer->length;
    if (!input_is_key_down(system->input, KEY_SHIFT))
      retained->text_selection = retained->text_cursor;
  }
  const String8 content = {.str = buffer->data, .length = buffer->length};
  if (!vkr_ui_text_prepare(system, node, content, &config->text))
    return false_v;
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

vkr_internal uint64_t vkr_ui_node_hash(VkrUiSystem *system,
                                       uint32_t node_index) {
  VkrUiFrameNode *node = &system->frame_nodes[node_index];
  uint64_t hash = vkr_ui_hash_bytes(UINT64_C(14695981039346656037), &node->kind,
                                    sizeof(node->kind));
  hash = vkr_ui_hash_bytes(hash, &node->id, sizeof(node->id));
  hash = vkr_ui_hash_bytes(hash, &node->placement, sizeof(node->placement));
  hash = vkr_ui_hash_bytes(hash, &node->style, sizeof(node->style));
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
  hash = vkr_ui_hash_bytes(hash, &node->hovered, sizeof(node->hovered));
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
  return (VkrUiGridItem){
      .column = placement.column,
      .row = placement.row,
      .column_span = Min(placement.column_span, columns),
      .row_span = Min(placement.row_span, rows),
      .justify = placement.justify,
      .align = placement.align,
      .intrinsic_size_px = child->intrinsic_size,
      .max_size_px = child->style.max_size_px,
      .margin_px = vkr_ui_edges_add(placement_margin, child->style.margin_px),
  };
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
      return false_v;

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
      return false_v;
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
  const VkrUiRect child_clip =
      node->clip_children ? vkr_ui_rect_intersect(parent_clip, content_rect)
                          : parent_clip;
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
    return false_v;
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
    return false_v;
  if (node->kind == VKR_UI_NODE_SCROLL) {
    const float32_t max_scroll =
        Max(0.0f, row_output.resolved_extent_px - content_rect.height);
    node->retained->scroll_offset.y =
        Min(node->retained->scroll_offset.y, max_scroll);
  }
  if (!vkr_ui_grid_arrange_items(
          content_rect,
          (VkrUiGridAxisView){column_offsets, column_sizes, columns},
          (VkrUiGridAxisView){row_offsets, row_sizes, rows}, items, child_count,
          occupancy, (uint32_t)cell_count, rects, child_count))
    return false_v;

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
    (void)vkr_ui_draw_buffer_rounded_rect(buffer, rect,
                                          vkr_ui_linear_color(color), radii);
  else
    (void)vkr_ui_draw_buffer_solid(buffer, rect, vkr_ui_linear_color(color));
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

vkr_internal void vkr_ui_emit_text(VkrUiSystem *system, VkrUiDrawBuffer *buffer,
                                   VkrUiFrameNode *node, VkrUiRect content_rect,
                                   bool8_t centered, float32_t x_offset) {
  VkrUiText *text = &node->retained->text;
  VkrFont *font = text->resolved_font;
  if (!node->retained->text_live || !font || font->atlas.id == 0u ||
      text->geometry.vertex_count == 0u)
    return;
  const VkrTextBounds bounds = vkr_ui_text_get_bounds(text);
  const float32_t origin_x =
      content_rect.x + x_offset +
      (centered
           ? Max(0.0f, content_rect.width - x_offset - bounds.size.x) * 0.5f
           : 0.0f);
  float32_t geometry_min_y = text->geometry.vertices[0].position.y;
  float32_t geometry_max_y = geometry_min_y;
  for (uint32_t vertex = 1u; vertex < text->geometry.vertex_count; ++vertex) {
    geometry_min_y =
        Min(geometry_min_y, text->geometry.vertices[vertex].position.y);
    geometry_max_y =
        Max(geometry_max_y, text->geometry.vertices[vertex].position.y);
  }
  const float32_t geometry_height = geometry_max_y - geometry_min_y;
  const float32_t top =
      content_rect.y + Max(0.0f, content_rect.height - geometry_height) * 0.5f;
  const VkrUiDrawMode mode = font->type == VKR_FONT_TYPE_MTSDF
                                 ? VKR_UI_DRAW_MODE_MTSDF_TEXT
                                 : VKR_UI_DRAW_MODE_BITMAP_TEXT;
  const float32_t screen_range = mode == VKR_UI_DRAW_MODE_MTSDF_TEXT
                                     ? vkr_ui_text_screen_range(text, font)
                                     : 0.0f;
  const Vec2 unit_range =
      mode == VKR_UI_DRAW_MODE_MTSDF_TEXT ? font->mtsdf_unit_range : (Vec2){0};
  const VkrUiTextureRef atlas = {font->atlas.id, font->atlas.generation};
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
}

vkr_internal VkrUiRect vkr_ui_uniform_inset(VkrUiRect rect, float32_t inset) {
  return vkr_ui_rect_inset(rect, (VkrUiEdges){inset, inset, inset, inset});
}

vkr_internal void vkr_ui_emit_node(VkrUiSystem *system, uint32_t node_index,
                                   VkrUiDrawBuffer *buffer) {
  VkrUiFrameNode *node = &system->frame_nodes[node_index];
  if (!vkr_ui_draw_buffer_push_clip(buffer, node->clip))
    return;
  node->draw_first_command = buffer->command_count;
  Vec4 background = node->style.background_color;
  if (node->kind == VKR_UI_NODE_BUTTON) {
    const float32_t factor = node->active    ? 0.68f
                             : node->hovered ? 1.18f
                                             : 1.0f;
    background.x = vkr_clamp_f32(background.x * factor, 0.0f, 1.0f);
    background.y = vkr_clamp_f32(background.y * factor, 0.0f, 1.0f);
    background.z = vkr_clamp_f32(background.z * factor, 0.0f, 1.0f);
  }
  VkrUiRect background_rect = node->rect;
  Vec4 background_radii = node->style.corner_radius_px;
  if (vkr_ui_edges_have_extent(node->style.border_px)) {
    vkr_ui_emit_rect(buffer, node->rect, node->style.border_color,
                     node->style.corner_radius_px);
    background_rect = vkr_ui_rect_inset(node->rect, node->style.border_px);
    background_radii =
        vkr_ui_inner_radii(node->style.corner_radius_px, node->style.border_px);
  }
  vkr_ui_emit_rect(buffer, background_rect, background, background_radii);
  const VkrUiRect content = vkr_ui_style_content_rect(node->rect, &node->style);
  switch (node->kind) {
  case VKR_UI_NODE_LABEL:
    vkr_ui_emit_text(system, buffer, node, content, false_v, 0.0f);
    break;
  case VKR_UI_NODE_BUTTON:
    vkr_ui_emit_text(system, buffer, node, content, true_v, 0.0f);
    break;
  case VKR_UI_NODE_CHECKBOX: {
    const float32_t size = Min(content.height, 16.0f * system->content_scale);
    const VkrUiRect box = {
        content.x, content.y + (content.height - size) * 0.5f, size, size};
    vkr_ui_emit_rect(buffer, box, (Vec4){0.22f, 0.24f, 0.28f, 1.0f},
                     (Vec4){2.0f, 2.0f, 2.0f, 2.0f});
    if (node->checked)
      vkr_ui_emit_rect(buffer, vkr_ui_uniform_inset(box, size * 0.22f),
                       (Vec4){0.20f, 0.65f, 1.0f, 1.0f},
                       (Vec4){1.0f, 1.0f, 1.0f, 1.0f});
    vkr_ui_emit_text(system, buffer, node, content, false_v,
                     size + 6.0f * system->content_scale);
    break;
  }
  case VKR_UI_NODE_SLIDER: {
    const float32_t track_height = Max(2.0f, 4.0f * system->content_scale);
    const VkrUiRect track = {
        content.x,
        content.y + (content.height - track_height) * 0.5f,
        content.width,
        track_height,
    };
    vkr_ui_emit_rect(buffer, track, (Vec4){0.18f, 0.20f, 0.24f, 1.0f},
                     (Vec4){track_height * 0.5f, track_height * 0.5f,
                            track_height * 0.5f, track_height * 0.5f});
    const float32_t knob = Min(content.height, 14.0f * system->content_scale);
    const VkrUiRect knob_rect = {
        content.x + node->slider_fraction * Max(0.0f, content.width - knob),
        content.y + (content.height - knob) * 0.5f,
        knob,
        knob,
    };
    vkr_ui_emit_rect(
        buffer, knob_rect, (Vec4){0.20f, 0.65f, 1.0f, 1.0f},
        (Vec4){knob * 0.5f, knob * 0.5f, knob * 0.5f, knob * 0.5f});
    break;
  }
  case VKR_UI_NODE_TEXT_FIELD:
    vkr_ui_emit_text(system, buffer, node, content, false_v, 0.0f);
    if (system->focused_id == node->id) {
      const VkrUiText *text = &node->retained->text;
      const String8 prefix = {
          .str = text->content.str,
          .length = Min(node->retained->text_cursor, text->content.length),
      };
      const uint64_t caret_glyph = vkr_string8_codepoint_count(&prefix);
      float32_t caret_x = 0.0f;
      if (caret_glyph < text->layout.glyphs.length)
        caret_x = text->layout.glyphs.data[caret_glyph].position.x;
      else if (text->layout.glyphs.length > 0u) {
        const VkrTextGlyph *last =
            &text->layout.glyphs.data[text->layout.glyphs.length - 1u];
        caret_x = last->position.x + last->advance;
      }
      const VkrTextBounds bounds =
          vkr_ui_text_get_bounds(&node->retained->text);
      const VkrUiRect cursor = {
          content.x + caret_x,
          content.y + Max(0.0f, (content.height - bounds.size.y) * 0.5f),
          Max(1.0f, system->content_scale),
          bounds.size.y,
      };
      vkr_ui_emit_rect(buffer, cursor, node->style.text_color, (Vec4){0});
    }
    break;
  default:
    break;
  }
  node->draw_command_count = buffer->command_count - node->draw_first_command;
  for (uint32_t child = node->first_child; child != VKR_UI_NODE_NONE;
       child = system->frame_nodes[child].next_sibling)
    vkr_ui_emit_node(system, child, buffer);
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
    estimate += 5u;
    if (node->retained->text_live)
      estimate += node->retained->text.geometry.vertex_count / 4u;
  }
  return (uint32_t)Min(estimate, (uint64_t)VKR_UI_INDEX_CAPACITY / 6u);
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

  VkrUiFrameNode *root = &system->frame_nodes[0];
  const uint64_t draw_hash = vkr_ui_node_hash(system, 0u);
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
    goto finish;
  }
  system->draw_cache_valid = false_v;
  if (!vkr_ui_container_intrinsic(system, root, &root->intrinsic_size)) {
    system->frame_open = false_v;
    return (VkrUiInputCapture){0};
  }
  const VkrUiRect target = {0.0f, 0.0f, (float32_t)system->target_width,
                            (float32_t)system->target_height};
  if (!vkr_ui_layout_node(system, 0u, target, target)) {
    system->frame_open = false_v;
    return (VkrUiInputCapture){0};
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

finish:
  system->capture = (VkrUiInputCapture){
      .mouse = system->capture.mouse || system->active_id != VKR_UI_ID_NONE,
      .keyboard = system->focused_id != VKR_UI_ID_NONE,
      .text = system->focused_id != VKR_UI_ID_NONE && system->focused_is_text,
      .hot_id = system->hot_id,
      .active_id = system->active_id,
  };
  vkr_ui_retained_reclaim(system);
  system->frame_open = false_v;
  return system->capture;
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
  if (!system->frame_draw_ready || target_width != system->target_width ||
      target_height != system->target_height)
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
