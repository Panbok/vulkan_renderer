#include "editor_debug_draw.h"

#include "core/vkr_debug_draw.h"
#include "editor_internal.h"
#include "editor_ui.h"

#define EDITOR_DEBUG_DRAW_LABEL_SIZE_PT 12.0f

/* The overlay's widgets from this frame's build, which the projection fills
   once the camera is final. The labels' text and anchors are copied at the
   build, so an operation that clears shapes later in the frame moves no
   widget onto another label's anchor. */
typedef struct EditorDebugDrawState {
  VkrUiId panel;
  VkrUiId path;
  uint32_t label_count;
  VkrUiId labels[VKR_DEBUG_DRAW_LABEL_MAX];
  Vec3 label_anchors[VKR_DEBUG_DRAW_LABEL_MAX];
  Vec2 label_sizes[VKR_DEBUG_DRAW_LABEL_MAX];
} EditorDebugDrawState;

vkr_global EditorDebugDrawState editor_debug_draw_state;

/* The named colours, as their names say rather than as the theme tints. */
vkr_global const Vec4 EDITOR_DEBUG_DRAW_PALETTE[VKR_DEBUG_DRAW_COLOR_COUNT] = {
    [VKR_DEBUG_DRAW_RED] = {1.0f, 0.25f, 0.22f, 1.0f},
    [VKR_DEBUG_DRAW_GREEN] = {0.3f, 0.9f, 0.35f, 1.0f},
    [VKR_DEBUG_DRAW_BLUE] = {0.3f, 0.55f, 1.0f, 1.0f},
    [VKR_DEBUG_DRAW_YELLOW] = {1.0f, 0.88f, 0.2f, 1.0f},
    [VKR_DEBUG_DRAW_CYAN] = {0.25f, 0.9f, 0.95f, 1.0f},
    [VKR_DEBUG_DRAW_MAGENTA] = {0.95f, 0.35f, 0.95f, 1.0f},
    [VKR_DEBUG_DRAW_WHITE] = {0.95f, 0.95f, 0.95f, 1.0f},
    [VKR_DEBUG_DRAW_ORANGE] = {1.0f, 0.6f, 0.15f, 1.0f},
};

_Static_assert(VKR_DEBUG_DRAW_COLOR_COUNT <= VKR_UI_PATH_PALETTE_CAPACITY,
               "every debug draw colour fits the path palette");

/* Clip distances of `p` to the view's six planes, with depth from 0 to 1. */
vkr_internal float32_t editor_debug_draw_clip_distance(Vec4 p, uint32_t plane) {
  switch (plane) {
  case 0:
    return p.w + p.x;
  case 1:
    return p.w - p.x;
  case 2:
    return p.w + p.y;
  case 3:
    return p.w - p.y;
  case 4:
    return p.z;
  default:
    return p.w - p.z;
  }
}

/* Projects `from`-`to` to Scene points, cut at the view's planes; false when
   nothing of it is in view. */
vkr_internal bool8_t editor_debug_draw_project_line(const Mat4 *view_projection,
                                                    Vec2 view, Vec3 from,
                                                    Vec3 to, Vec2 *start,
                                                    Vec2 *end) {
  Vec4 a = mat4_mul_vec4(*view_projection, vec3_to_vec4(from, 1.0f));
  Vec4 b = mat4_mul_vec4(*view_projection, vec3_to_vec4(to, 1.0f));
  for (uint32_t plane = 0u; plane < 6u; ++plane) {
    const float32_t da = editor_debug_draw_clip_distance(a, plane);
    const float32_t db = editor_debug_draw_clip_distance(b, plane);
    if (!vkr_is_finite_f32(da) || !vkr_is_finite_f32(db) ||
        (da < 0.0f && db < 0.0f)) {
      return false_v;
    }
    if ((da < 0.0f) != (db < 0.0f)) {
      const Vec4 intersection =
          vec4_add(a, vec4_scale(vec4_sub(b, a), da / (da - db)));
      if (da < 0.0f) {
        a = intersection;
      } else {
        b = intersection;
      }
    }
  }
  if (a.w <= 1.0e-6f || b.w <= 1.0e-6f) {
    return false_v;
  }
  *start = (Vec2){view.x * (a.x / a.w * 0.5f + 0.5f),
                  view.y * (a.y / a.w * 0.5f + 0.5f)};
  *end = (Vec2){view.x * (b.x / b.w * 0.5f + 0.5f),
                view.y * (b.y / b.w * 0.5f + 0.5f)};
  return true_v;
}

void vkr_editor_debug_draw_build(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame) {
  (void)editor;
  EditorDebugDrawState *state = &editor_debug_draw_state;
  state->panel = VKR_UI_ID_NONE;
  state->path = VKR_UI_ID_NONE;
  state->label_count = 0u;
  const VkrDebugDraw *draw = frame->debug_draw;
  /* A Pass output view shows a graph image the world does not land on. */
  if (!draw || (!draw->segment_count && !draw->label_count) ||
      !frame->mapping_valid || frame->scene_rendering_stopped ||
      frame->view_state.pass_output != 0u || (!frame->scene && !frame->world)) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const Vec4 image = frame->mapping.image_rect_px;
  const Vec2 view = {image.z / ui->content_scale, image.w / ui->content_scale};
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = VKR_UI_PLACEMENT_DEFAULT;
  panel.placement.column = panel.placement.row = 0;
  panel.placement.justify = panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt = (VkrUiEdges){image.y / ui->content_scale, 0, 0,
                                           image.x / ui->content_scale};
  panel.style.min_size_pt = panel.style.max_size_pt = view;
  panel.style.padding_pt = (VkrUiEdges){0};
  panel.style.background_color = (Vec4){0};
  panel.clip_children = true_v;
  state->panel = vkr_ui_id_stack_widget_label(&ui->id_stack,
                                              string8_lit("editor.debug_draw"));
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.debug_draw"), &panel)) {
    state->panel = VKR_UI_ID_NONE;
    return;
  }
  if (draw->segment_count) {
    VkrUiWidgetConfig config = vkr_ui_widget_config_default();
    config.placement.column = config.placement.row = 0;
    config.placement.justify = config.placement.align = VKR_UI_ALIGN_START;
    config.style.min_size_pt = config.style.max_size_pt = view;
    state->path = vkr_ui_path(ui, string8_lit("lines"), NULL, &config);
  }
  /* Labels start out of sight; the projection places them. */
  const VkrUiTheme *theme = vkr_ui_theme();
  for (uint32_t i = 0u; i < draw->label_count; ++i) {
    const VkrDebugDrawLabel *label = &draw->labels[i];
    const char *text = label->text;
    const Vec2 measured = vkr_editor_text_size(
        ui, VKR_FONT_HANDLE_INVALID, text, EDITOR_DEBUG_DRAW_LABEL_SIZE_PT);
    const Vec2 size = {vkr_ceil_f32(measured.x) + 14.0f,
                       vkr_ceil_f32(measured.y) + 6.0f};
    VkrUiWidgetConfig config = vkr_ui_widget_config_default();
    config.placement.column = config.placement.row = 0;
    config.placement.justify = config.placement.align = VKR_UI_ALIGN_START;
    config.placement.margin_pt = (VkrUiEdges){100000.0f, 0, 0, 100000.0f};
    config.style.min_size_pt = config.style.max_size_pt = size;
    config.style.padding_pt = (VkrUiEdges){3, 7, 3, 7};
    config.style.font_size_pt = EDITOR_DEBUG_DRAW_LABEL_SIZE_PT;
    config.style.text_color = EDITOR_DEBUG_DRAW_PALETTE[label->style.color];
    config.style.background_color = vkr_ui_color_alpha(theme->overlay, 0.85f);
    config.style.corner_radius_pt = (Vec4){3, 3, 3, 3};
    (void)vkr_ui_push_id_u64(ui, i);
    vkr_ui_label(
        ui, string8_lit("label"),
        string8_create_from_cstr((const uint8_t *)text, string_length(text)),
        &config);
    const uint32_t index = state->label_count++;
    state->labels[index] =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("label"));
    state->label_anchors[index] = label->at;
    state->label_sizes[index] = size;
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

void vkr_editor_debug_draw_project(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame) {
  (void)editor;
  const EditorDebugDrawState *state = &editor_debug_draw_state;
  const VkrDebugDraw *draw = frame->debug_draw;
  if (state->panel == VKR_UI_ID_NONE || !draw) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const Vec4 image = frame->mapping.image_rect_px;
  const float32_t scale = ui->content_scale;
  const Vec2 view = {image.z / scale, image.w / scale};
  (void)vkr_ui_widget_set_rect(
      ui, state->panel,
      (VkrUiRect){image.x / scale, image.y / scale, view.x, view.y});
  /* Shapes are in level coordinates; a Play rebase moves the rendered world
     by the level's origin offset. */
  const VkrScene *level = frame->scene ? frame->scene : frame->world;
  const Vec3 origin = level ? level->origin_offset : vec3_zero();
  const Mat4 view_projection =
      mat4_mul(frame->view_projection, mat4_translate(vec3_negate(origin)));
  for (uint32_t i = 0u; i < state->label_count; ++i) {
    const Vec4 clip = mat4_mul_vec4(
        view_projection, vec3_to_vec4(state->label_anchors[i], 1.0f));
    const Vec2 size = state->label_sizes[i];
    VkrUiRect rect = {100000.0f, 100000.0f, size.x, size.y};
    if (clip.w > 1.0e-6f && clip.z >= 0.0f && clip.z <= clip.w) {
      const Vec2 at = {view.x * (clip.x / clip.w * 0.5f + 0.5f),
                       view.y * (clip.y / clip.w * 0.5f + 0.5f)};
      if (at.x >= 0.0f && at.y >= 0.0f && at.x <= view.x && at.y <= view.y) {
        rect.x = vkr_clamp_f32(at.x - rect.width * 0.5f, 0.0f,
                               Max(0.0f, view.x - rect.width));
        rect.y = vkr_clamp_f32(at.y - rect.height - 4.0f, 0.0f,
                               Max(0.0f, view.y - rect.height));
      }
    }
    (void)vkr_ui_widget_set_rect(ui, state->labels[i], rect);
  }
  if (state->path == VKR_UI_ID_NONE) {
    return;
  }
  /* Each segment takes its two points and a break, except one that goes on
     from the previous segment's end, as a sphere's circle does. */
  const uint32_t capacity =
      Min(VKR_UI_PATH_POINT_MAX, draw->segment_count * 3u);
  Vec2 *points = vkr_allocator_alloc(ui->frame_allocator,
                                     sizeof(Vec2) * (uint64_t)capacity,
                                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint8_t *colors = vkr_allocator_alloc(ui->frame_allocator, capacity,
                                        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!points || !colors) {
    return;
  }
  const Vec2 gap = {VKR_FLOAT_INFINITY, VKR_FLOAT_INFINITY};
  uint32_t count = 0u;
  bool8_t open = false_v;
  Vec2 open_end = {0};
  for (uint32_t i = 0u; i < draw->segment_count && count + 3u <= capacity;
       ++i) {
    const VkrDebugDrawSegment *segment = &draw->segments[i];
    Vec2 start;
    Vec2 end;
    if (!editor_debug_draw_project_line(&view_projection, view, segment->from,
                                        segment->to, &start, &end)) {
      open = false_v;
      continue;
    }
    const uint8_t color = (uint8_t)segment->style.color;
    const bool8_t chained =
        open && i > 0u &&
        draw->segments[i - 1u].style.color == segment->style.color &&
        start.x == open_end.x && start.y == open_end.y;
    if (!chained) {
      if (count > 0u) {
        colors[count] = color;
        points[count++] = gap;
      }
      colors[count] = color;
      points[count++] = start;
    }
    colors[count] = color;
    points[count++] = end;
    open = true_v;
    open_end = end;
  }
  const VkrUiPathConfig path = {
      .points = points,
      .colors = colors,
      .count = count,
      .palette = EDITOR_DEBUG_DRAW_PALETTE,
      .palette_count = ArrayCount(EDITOR_DEBUG_DRAW_PALETTE),
      .width_pt = 1.5f,
  };
  (void)vkr_ui_path_set(ui, state->path, &path);
}
