#include "editor_recording.h"

#include "containers/vkr_sort.h"
#include "editor_internal.h"
#include "editor_ui.h"

/* The Performance window's Draws tab: a top-down map of the level with the
   camera's path coloured by frame time, its frustum at the shown frame, and
   every object the GPU's camera culling tested, coloured by what it did
   with it (the draw census); beside it the frame's culling funnel and its
   draw counters or cells, and below the culling over the shown frames. */

/* Map layers, as bits of VkrEditorRecordingPerfView.draws_layers minus one. */
enum {
  DRAWS_LAYER_PATH = 1u << 0u,
  DRAWS_LAYER_FRUSTUM = 1u << 1u,
  DRAWS_LAYER_DRAWN = 1u << 2u,
  DRAWS_LAYER_OCCLUDED = 1u << 3u,
  DRAWS_LAYER_CULLED = 1u << 4u,
  DRAWS_LAYER_CELLS = 1u << 5u,
  DRAWS_LAYER_DEFAULT = DRAWS_LAYER_PATH | DRAWS_LAYER_FRUSTUM |
                        DRAWS_LAYER_DRAWN | DRAWS_LAYER_OCCLUDED |
                        DRAWS_LAYER_CULLED,
};

static const struct {
  const char *label;
  const char *tip;
  uint32_t bit;
} DRAWS_LAYERS[] = {
    {"Path", "The camera's path, coloured by frame time", DRAWS_LAYER_PATH},
    {"Frustum", "The camera's view at the shown frame", DRAWS_LAYER_FRUSTUM},
    {"Drawn", "Objects the frame drew", DRAWS_LAYER_DRAWN},
    {"Occluded", "Objects in view that the depth pyramid hid",
     DRAWS_LAYER_OCCLUDED},
    {"Culled", "Objects outside the frustum or past the draw distance",
     DRAWS_LAYER_CULLED},
    {"Cells", "Drawn objects per cell of the ground", DRAWS_LAYER_CELLS},
};

/* State colours, by VKR_RECORDING_DRAW_STATE_*: not a camera candidate,
   drawn, drawn late, occluded, outside the frustum, past the draw
   distance; then the path's three frame-time classes. */
enum {
  DRAWS_COLOR_NONE = 0,
  DRAWS_COLOR_DRAWN,
  DRAWS_COLOR_LATE,
  DRAWS_COLOR_OCCLUDED,
  DRAWS_COLOR_FRUSTUM,
  DRAWS_COLOR_DISTANCE,
  DRAWS_COLOR_COUNT,
};
static const Vec4 DRAWS_STATE_COLOR[DRAWS_COLOR_COUNT] = {
    {0.150f, 0.155f, 0.170f, 1.0f}, {0.224f, 0.529f, 0.898f, 1.0f},
    {0.565f, 0.522f, 0.914f, 1.0f}, {0.788f, 0.522f, 0.000f, 1.0f},
    {0.200f, 0.208f, 0.231f, 1.0f}, {0.165f, 0.173f, 0.188f, 1.0f},
};
static const char *const DRAWS_STATE_NAME[DRAWS_COLOR_COUNT] = {
    "not a camera candidate",    "drawn",
    "deferred, then drawn late", "in view, occluded by the depth pyramid",
    "outside the frustum",       "past the draw distance or too small",
};
/* Over time: drawn, drawn late, occluded, frustum, distance. */
static const char *const DRAWS_SERIES_LABEL[VKR_EDITOR_PERF_DRAW_SERIES] = {
    "Drawn", "Drawn late", "Occluded", "Outside frustum", "Distance"};

/* Objects whose footprint covers more than this share of the level draw
   faintly under the rest, so a ground plane does not hide them. */
#define DRAWS_LARGE_SHARE 0.05f
/* Path points at most, cells labelled at most, and the default cell size
   in metres without a world partition. */
#define DRAWS_PATH_POINTS 4096u
#define DRAWS_CELL_LABELS 64u
#define DRAWS_CELL_DEFAULT 32.0f
/* Frames a click on the path may be from the pointer, in points. */
#define DRAWS_PICK_PT 14.0f

/* Most visible first: drawn, drawn late, occluded, frustum, distance, not
   a candidate. */
static const uint8_t DRAWS_RANK[DRAWS_COLOR_COUNT] = {5u, 0u, 1u, 2u, 3u, 4u};

/* ------------------------------------------------------------------------ */
/* Data                                                                      */

void vkr_editor_recording_perf_draws_release(VkrEditorRecordingPerf *perf) {
  VkrEditorPerfDraws *draws = &perf->draws;
  if (draws->order) {
    vkr_allocator_free(&perf->allocator, draws->order,
                       sizeof(uint32_t) * draws->object_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (draws->object_state) {
    vkr_allocator_free(&perf->allocator, draws->object_state,
                       draws->object_count, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (draws->candidate_object) {
    vkr_allocator_free(&perf->allocator, draws->candidate_object,
                       sizeof(uint32_t) * draws->candidate_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  draws->order = NULL;
  draws->object_state = NULL;
  draws->candidate_object = NULL;
  draws->object_count = 0u;
  draws->candidate_count = 0u;
  draws->bounds = UINT32_MAX;
  draws->ids = UINT32_MAX;
}

typedef struct DrawsArea {
  uint32_t index;
  float32_t area;
} DrawsArea;

vkr_internal int32_t draws_compare_area(const void *lhs, const void *rhs) {
  const DrawsArea *a = lhs;
  const DrawsArea *b = rhs;
  if (a->area != b->area) {
    return a->area > b->area ? -1 : 1;
  }
  return a->index < b->index ? -1 : (a->index > b->index ? 1 : 0);
}

vkr_internal float32_t draws_footprint(const VkrRecordingDrawObject *object) {
  return Max(object->max[0] - object->min[0], 0.0f) *
         Max(object->max[2] - object->min[2], 0.0f);
}

/* Takes box set `bounds`: the objects' order by footprint and the level's
   extent. False without memory. */
vkr_internal bool8_t draws_take_bounds(VkrEditorRecordingPerf *perf,
                                       uint32_t bounds) {
  VkrEditorPerfDraws *draws = &perf->draws;
  if (draws->bounds == bounds && draws->order) {
    return true_v;
  }
  vkr_editor_recording_perf_draws_release(perf);
  const VkrRecordingDrawBounds *set = &perf->data.draw_bounds[bounds];
  const uint32_t count = set->object_count;
  draws->order = vkr_allocator_alloc(&perf->allocator, sizeof(uint32_t) * count,
                                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  draws->object_state = vkr_allocator_alloc(&perf->allocator, count,
                                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  DrawsArea *areas =
      vkr_allocator_alloc(&perf->allocator, sizeof(DrawsArea) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  draws->object_count = count;
  if (!draws->order || !draws->object_state || !areas) {
    if (areas) {
      vkr_allocator_free(&perf->allocator, areas, sizeof(DrawsArea) * count,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    vkr_editor_recording_perf_draws_release(perf);
    return false_v;
  }
  draws->bounds = bounds;
  draws->state_frame = UINT64_MAX;
  draws->extent_min = (Vec2){1.0e30f, 1.0e30f};
  draws->extent_max = (Vec2){-1.0e30f, -1.0e30f};
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrRecordingDrawObject *object = &set->objects[i];
    areas[i] = (DrawsArea){.index = i, .area = draws_footprint(object)};
    if (vkr_is_finite_f32(object->min[0]) &&
        vkr_is_finite_f32(object->max[2])) {
      draws->extent_min.x = Min(draws->extent_min.x, object->min[0]);
      draws->extent_min.y = Min(draws->extent_min.y, object->min[2]);
      draws->extent_max.x = Max(draws->extent_max.x, object->max[0]);
      draws->extent_max.y = Max(draws->extent_max.y, object->max[2]);
    }
  }
  vkr_sort(areas, count, sizeof(DrawsArea), draws_compare_area);
  for (uint32_t i = 0u; i < count; ++i) {
    draws->order[i] = areas[i].index;
  }
  vkr_allocator_free(&perf->allocator, areas, sizeof(DrawsArea) * count,
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  /* The camera's path widens the extent. */
  const VkrRecordingData *data = &perf->data;
  for (uint64_t f = 0u; f < data->frame_count; ++f) {
    const float32_t x = (float32_t)data->positions[f * 3u];
    const float32_t z = (float32_t)data->positions[f * 3u + 2u];
    draws->extent_min =
        (Vec2){Min(draws->extent_min.x, x), Min(draws->extent_min.y, z)};
    draws->extent_max =
        (Vec2){Max(draws->extent_max.x, x), Max(draws->extent_max.y, z)};
  }
  return true_v;
}

/* The object index of `object_id` in a set sorted by id, or UINT32_MAX. */
vkr_internal uint32_t draws_find(const VkrRecordingDrawBounds *set,
                                 uint32_t object_id) {
  uint32_t low = 0u;
  uint32_t high = set->object_count;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2u;
    const uint32_t id = set->objects[middle].object_id;
    if (id == object_id) {
      return middle;
    }
    if (id < object_id) {
      low = middle + 1u;
    } else {
      high = middle;
    }
  }
  return UINT32_MAX;
}

/* Each candidate of `census`'s id table mapped to its object. */
vkr_internal bool8_t draws_take_ids(VkrEditorRecordingPerf *perf,
                                    const VkrRecordingDrawFrame *census) {
  VkrEditorPerfDraws *draws = &perf->draws;
  const uint32_t table = (uint32_t)(census->ids - perf->data.draw_ids);
  if (draws->ids == table && draws->candidate_object) {
    return true_v;
  }
  if (draws->candidate_object) {
    vkr_allocator_free(&perf->allocator, draws->candidate_object,
                       sizeof(uint32_t) * draws->candidate_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  const uint32_t count = census->ids->count;
  draws->candidate_object =
      vkr_allocator_alloc(&perf->allocator, sizeof(uint32_t) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  draws->candidate_count = draws->candidate_object ? count : 0u;
  draws->ids = draws->candidate_object ? table : UINT32_MAX;
  if (!draws->candidate_object) {
    return false_v;
  }
  const VkrRecordingDrawBounds *set = &perf->data.draw_bounds[draws->bounds];
  for (uint32_t c = 0u; c < count; ++c) {
    draws->candidate_object[c] = draws_find(set, census->ids->ids[c]);
  }
  return true_v;
}

/* Each object's most visible state in frame `frame`. */
vkr_internal void draws_take_frame(VkrEditorRecordingPerf *perf,
                                   uint64_t frame) {
  VkrEditorPerfDraws *draws = &perf->draws;
  const VkrRecordingData *data = &perf->data;
  const uint32_t bounds = vkr_recording_data_draw_bounds_at(data, frame);
  if (bounds == UINT32_MAX || !draws_take_bounds(perf, bounds)) {
    return;
  }
  if (draws->state_frame == frame) {
    return;
  }
  draws->state_frame = frame;
  MemSet(draws->object_state, UINT8_MAX, draws->object_count);
  MemZero(draws->state_counts, sizeof(draws->state_counts));
  VkrRecordingDrawFrame census;
  if (!vkr_recording_data_draw_frame(data, frame, &census) ||
      !draws_take_ids(perf, &census)) {
    return;
  }
  const uint32_t count = Min(census.count, draws->candidate_count);
  for (uint32_t c = 0u; c < count; ++c) {
    const uint32_t state = vkr_recording_draw_state(census.words, c);
    if (state > VKR_RECORDING_DRAW_STATE_DISTANCE) {
      continue;
    }
    ++draws->state_counts[state];
    const uint32_t object = draws->candidate_object[c];
    if (object >= draws->object_count) {
      continue;
    }
    uint8_t *known = &draws->object_state[object];
    if (*known == UINT8_MAX || DRAWS_RANK[state] < DRAWS_RANK[*known]) {
      *known = (uint8_t)state;
    }
  }
}

/* The chart: each column's candidates by state. */
vkr_internal void draws_update(VkrEditorRecordingPerf *perf) {
  VkrEditorPerfDraws *draws = &perf->draws;
  if (draws->valid && draws->columns_version == perf->columns_version) {
    return;
  }
  draws->columns_version = perf->columns_version;
  draws->valid = true_v;
  draws->top = 1.0f;
  const VkrRecordingData *data = &perf->data;
  for (uint32_t c = 0u; c < perf->column_count; ++c) {
    uint32_t counts[VKR_RECORDING_DRAW_STATE_DISTANCE + 1u] = {0};
    VkrRecordingDrawFrame census;
    const uint64_t frame = perf->column_frame[c];
    if (vkr_recording_data_draw_frame(data, frame, &census)) {
      for (uint32_t i = 0u; i < census.count; ++i) {
        const uint32_t state = vkr_recording_draw_state(census.words, i);
        if (state <= VKR_RECORDING_DRAW_STATE_DISTANCE) {
          ++counts[state];
        }
      }
    }
    float32_t total = 0.0f;
    for (uint32_t s = 0u; s < VKR_EDITOR_PERF_DRAW_SERIES; ++s) {
      draws->counts[s][c] = (float32_t)counts[1u + s];
      total += draws->counts[s][c];
    }
    draws->top = Max(draws->top, total);
  }
  draws->top *= 1.06f;
}

/* ------------------------------------------------------------------------ */
/* Map                                                                       */

vkr_internal uint32_t draws_layers(const VkrEditorRecordingPerfView *view) {
  return view->draws_layers == 0u ? (uint32_t)DRAWS_LAYER_DEFAULT
                                  : view->draws_layers - 1u;
}

/* Whether objects in `state` show with `layers`. */
vkr_internal bool8_t draws_state_shown(uint32_t layers, uint8_t state) {
  switch (state) {
  case VKR_RECORDING_DRAW_STATE_DRAWN:
  case VKR_RECORDING_DRAW_STATE_DRAWN_LATE:
    return (layers & DRAWS_LAYER_DRAWN) != 0u;
  case VKR_RECORDING_DRAW_STATE_OCCLUDED:
    return (layers & DRAWS_LAYER_OCCLUDED) != 0u;
  default:
    return (layers & DRAWS_LAYER_CULLED) != 0u;
  }
}

/* The map's toolbar: what it shows at the frame, the layers, Follow and
   Fit. */
vkr_internal void draws_toolbar(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame,
                                VkrUiPlacement placement, uint64_t at) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrEditorRecordingPerfView *view = &perf->view;
  const VkrRecordingData *data = &perf->data;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiTrack columns[2u + ArrayCount(DRAWS_LAYERS) + 2u];
  columns[0] = fill;
  for (uint32_t i = 1u; i < ArrayCount(columns); ++i) {
    columns[i] = auto_track;
  }
  VkrUiPanelConfig row =
      vkr_editor_perf_grid(placement, columns, ArrayCount(columns), &fill, 1u);
  row.style.gap_pt = theme->space_xs;
  if (!vkr_ui_panel_begin(ui, string8_lit("draws.toolbar"), &row)) {
    return;
  }
  const float64_t *position = &data->positions[at * 3u];
  vkr_editor_perf_caption(
      ui, "where",
      string8_create_formatted(ui->frame_allocator,
                               "Top down  \xc2\xb7  frame %llu  \xc2\xb7  "
                               "camera %.1f, %.1f  \xc2\xb7  yaw %.0f\xc2\xb0",
                               (unsigned long long)at, position[0], position[2],
                               (float64_t)data->yaws[at]),
      vkr_editor_perf_cell(0u, 0u), theme->text_secondary);
  uint32_t layers = draws_layers(view);
  for (uint32_t i = 0u; i < ArrayCount(DRAWS_LAYERS); ++i) {
    const bool8_t on = (layers & DRAWS_LAYERS[i].bit) != 0u;
    VkrUiWidgetConfig chip = vkr_ui_widget_config_default();
    chip.placement = vkr_editor_perf_cell(1u + i, 0u);
    chip.placement.align = VKR_UI_ALIGN_CENTER;
    vkr_editor_ghost_style(&chip);
    vkr_editor_toggle_style(&chip, on);
    chip.style.font_size_pt = theme->font_caption;
    chip.style.padding_pt = (VkrUiEdges){2.0f, 8.0f, 2.0f, 8.0f};
    chip.tooltip =
        string8_create_from_cstr((const uint8_t *)DRAWS_LAYERS[i].tip,
                                 string_length(DRAWS_LAYERS[i].tip));
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(
            ui, string8_lit("layer"),
            string8_create_from_cstr((const uint8_t *)DRAWS_LAYERS[i].label,
                                     string_length(DRAWS_LAYERS[i].label)),
            &chip)) {
      layers ^= DRAWS_LAYERS[i].bit;
    }
    (void)vkr_ui_pop_id(ui);
  }
  view->draws_layers = layers + 1u;
  const uint32_t follow_column = 1u + ArrayCount(DRAWS_LAYERS);
  VkrUiWidgetConfig follow = vkr_ui_widget_config_default();
  follow.placement = vkr_editor_perf_cell(follow_column, 0u);
  follow.placement.align = VKR_UI_ALIGN_CENTER;
  vkr_editor_ghost_style(&follow);
  vkr_editor_toggle_style(&follow, view->draws_follow);
  follow.style.font_size_pt = theme->font_caption;
  follow.style.padding_pt = (VkrUiEdges){2.0f, 8.0f, 2.0f, 6.0f};
  follow.icon = VKR_UI_ICON_CROSSHAIR;
  follow.icon_size_pt = 12.0f;
  follow.tooltip = string8_lit("Keep the camera in the middle of the map");
  if (vkr_ui_button(ui, string8_lit("follow"), string8_lit("Follow"),
                    &follow)) {
    view->draws_follow = !view->draws_follow;
  }
  VkrUiWidgetConfig fit = follow;
  fit.placement = vkr_editor_perf_cell(follow_column + 1u, 0u);
  vkr_editor_toggle_style(&fit, false_v);
  fit.icon = VKR_UI_ICON_FRAME;
  fit.tooltip = string8_lit("Fit the whole level in view");
  if (vkr_ui_button(ui, string8_lit("fit"), string8_lit("Fit"), &fit)) {
    view->draws_canvas.fitted = false_v;
    view->draws_follow = false_v;
  }
  (void)vkr_ui_panel_end(ui);
}

/* Map points: world x and z on the canvas. */
vkr_internal Vec2 draws_point(const VkrEditorPerfCanvas *canvas, float32_t x,
                              float32_t z) {
  return vkr_editor_perf_canvas_point(canvas, (Vec2){x, z});
}

/* The objects as filled footprints, the large ones faintly under the rest;
   a widget holds at most VKR_UI_PATH_FILL_MAX of them. */
vkr_internal void draws_objects(VkrEditorRecordingPerf *perf, VkrUiSystem *ui,
                                uint32_t layers) {
  const VkrEditorPerfDraws *draws = &perf->draws;
  const VkrEditorPerfCanvas *canvas = &perf->view.draws_canvas;
  const VkrRecordingDrawBounds *set = &perf->data.draw_bounds[draws->bounds];
  const float32_t level = Max((draws->extent_max.x - draws->extent_min.x) *
                                  (draws->extent_max.y - draws->extent_min.y),
                              1.0f);
  VkrUiRect *fills = vkr_allocator_alloc(
      ui->frame_allocator, sizeof(VkrUiRect) * VKR_UI_PATH_FILL_MAX,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint8_t *colors =
      vkr_allocator_alloc(ui->frame_allocator, VKR_UI_PATH_FILL_MAX,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!fills || !colors) {
    return;
  }
  Vec4 faint[DRAWS_COLOR_COUNT];
  for (uint32_t i = 0u; i < DRAWS_COLOR_COUNT; ++i) {
    faint[i] = vkr_ui_color_alpha(DRAWS_STATE_COLOR[i], 0.10f);
  }
  uint32_t widget = 0u;
  uint32_t count = 0u;
  bool8_t large = true_v;
  for (uint32_t k = 0u; k <= draws->object_count; ++k) {
    const bool8_t done = k == draws->object_count;
    const uint32_t o = done ? 0u : draws->order[k];
    const VkrRecordingDrawObject *object = &set->objects[o];
    const bool8_t now_large =
        !done && draws_footprint(object) > DRAWS_LARGE_SHARE * level;
    /* A full widget, the end, or the switch from large to small objects
       draws what was gathered. */
    if (count == VKR_UI_PATH_FILL_MAX || done || (large && !now_large)) {
      if (count > 0u) {
        const VkrUiPathConfig path = {
            .palette = large ? faint : DRAWS_STATE_COLOR,
            .palette_count = DRAWS_COLOR_COUNT,
            .width_pt = 1.0f,
            .fills = fills,
            .fill_colors = colors,
            .fill_count = count,
        };
        VkrUiWidgetConfig config = vkr_ui_widget_config_default();
        config.placement = vkr_editor_perf_cell(0u, 0u);
        (void)vkr_ui_push_id_u64(ui, widget++);
        (void)vkr_ui_path(ui, string8_lit("objects"), &path, &config);
        (void)vkr_ui_pop_id(ui);
      }
      count = 0u;
      large = now_large;
      if (done) {
        break;
      }
    }
    const uint8_t state = draws->object_state[o];
    const uint8_t shown = state == UINT8_MAX
                              ? (uint8_t)VKR_RECORDING_DRAW_STATE_NOT_CAMERA
                              : state;
    if (!draws_state_shown(layers, shown)) {
      continue;
    }
    const Vec2 a = draws_point(canvas, object->min[0], object->min[2]);
    const Vec2 b = draws_point(canvas, object->max[0], object->max[2]);
    if (b.x < 0.0f || b.y < 0.0f || a.x > canvas->size_pt.x ||
        a.y > canvas->size_pt.y) {
      continue;
    }
    /* A hair's gap keeps neighbours apart; the smallest stay a dot. */
    const float32_t inset = (b.x - a.x) > 3.0f ? 0.5f : 0.0f;
    fills[count] = (VkrUiRect){a.x + inset, a.y + inset,
                               Max(b.x - a.x - 2.0f * inset, 1.0f),
                               Max(b.y - a.y - 2.0f * inset, 1.0f)};
    colors[count++] = shown;
  }
}

/* The camera's path coloured by frame time, the shown frames bright and
   the rest dim. */
vkr_internal void draws_path(VkrEditorRecordingPerf *perf, VkrUiSystem *ui) {
  const VkrRecordingData *data = &perf->data;
  const VkrEditorPerfCanvas *canvas = &perf->view.draws_canvas;
  const VkrUiTheme *theme = vkr_ui_theme();
  const uint64_t frames = data->frame_count;
  const uint64_t stride =
      Max((frames + DRAWS_PATH_POINTS - 1u) / DRAWS_PATH_POINTS, 1ull);
  const uint32_t capacity = (uint32_t)(frames / stride + 2u);
  Vec2 *points =
      vkr_allocator_alloc(ui->frame_allocator, sizeof(Vec2) * capacity,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint8_t *colors = vkr_allocator_alloc(ui->frame_allocator, capacity,
                                        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!points || !colors) {
    return;
  }
  const float64_t budget = perf->analysis.budget_ms;
  uint32_t count = 0u;
  for (uint64_t f = 0u; f < frames && count < capacity; f += stride) {
    /* Each point takes the worst frame of its stride. */
    float64_t worst = -1.0;
    for (uint64_t k = f; k < Min(f + stride, frames); ++k) {
      float64_t ms = 0.0;
      if (vkr_recording_data_frame_ms(data, k, &ms)) {
        worst = Max(worst, ms);
      }
    }
    points[count] = draws_point(canvas, (float32_t)data->positions[f * 3u],
                                (float32_t)data->positions[f * 3u + 2u]);
    const bool8_t shown = f >= perf->view_first && f <= perf->view_last;
    colors[count++] = !shown                 ? 3u
                      : worst > 2.0 * budget ? 2u
                      : worst > budget       ? 1u
                                             : 0u;
  }
  const Vec4 palette[] = {theme->success, theme->warning, theme->error,
                          vkr_ui_color_alpha(theme->text_secondary, 0.35f)};
  const VkrUiPathConfig path = {
      .points = points,
      .colors = colors,
      .count = count,
      .palette = palette,
      .palette_count = ArrayCount(palette),
      .width_pt = 2.5f,
  };
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement = vkr_editor_perf_cell(0u, 0u);
  (void)vkr_ui_path(ui, string8_lit("path"), &path, &config);
}

/* The camera at frame `at`: its frustum's two side edges and far arc, and
   its position. */
vkr_internal void draws_frustum(VkrEditorRecordingPerf *perf, VkrUiSystem *ui,
                                uint64_t at, uint32_t layers) {
  const VkrRecordingData *data = &perf->data;
  const VkrEditorPerfCanvas *canvas = &perf->view.draws_canvas;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t x = (float32_t)data->positions[at * 3u];
  const float32_t z = (float32_t)data->positions[at * 3u + 2u];
  /* VkrCamera looks along (cos yaw, sin yaw) on the ground plane. */
  const float32_t yaw = vkr_to_radians(data->yaws[at]);
  const float32_t fov = data->fovs[at] > 1.0f ? data->fovs[at] : 60.0f;
  const float32_t aspect =
      data->output_height > 0u
          ? (float32_t)data->output_width / (float32_t)data->output_height
          : 16.0f / 9.0f;
  const float32_t half =
      vkr_atan_f32(vkr_tan_f32(vkr_to_radians(fov) * 0.5f) * aspect);
  /* The far plane, but no further than the level's size. */
  const float32_t level =
      Max(perf->draws.extent_max.x - perf->draws.extent_min.x,
          perf->draws.extent_max.y - perf->draws.extent_min.y);
  const float32_t far_clip =
      data->far_clips[at] > 0.0f ? data->far_clips[at] : level;
  const float32_t reach = Min(far_clip, Max(level * 0.6f, 20.0f));
  if (layers & DRAWS_LAYER_FRUSTUM) {
    enum { ARC = 24 };
    Vec2 points[ARC + 3];
    uint32_t count = 0u;
    points[count++] = draws_point(canvas, x, z);
    for (uint32_t i = 0u; i <= ARC; ++i) {
      const float32_t angle =
          yaw - half + 2.0f * half * (float32_t)i / (float32_t)ARC;
      points[count++] = draws_point(canvas, x + vkr_cos_f32(angle) * reach,
                                    z + vkr_sin_f32(angle) * reach);
    }
    points[count++] = points[0];
    const Vec4 palette[] = {vkr_ui_color_alpha(theme->accent_hover, 0.85f)};
    const VkrUiPathConfig path = {
        .points = points,
        .count = count,
        .palette = palette,
        .palette_count = 1u,
        .width_pt = 1.5f,
    };
    VkrUiWidgetConfig config = vkr_ui_widget_config_default();
    config.placement = vkr_editor_perf_cell(0u, 0u);
    (void)vkr_ui_path(ui, string8_lit("frustum"), &path, &config);
  }
  /* The camera: a dot and its heading. */
  const Vec2 eye = draws_point(canvas, x, z);
  const Vec2 heading[] = {
      eye,
      {eye.x + vkr_cos_f32(yaw) * 14.0f, eye.y + vkr_sin_f32(yaw) * 14.0f}};
  const Vec4 white[] = {theme->text};
  const VkrUiPathConfig arrow = {
      .points = heading,
      .count = 2u,
      .palette = white,
      .palette_count = 1u,
      .width_pt = 2.0f,
  };
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement = vkr_editor_perf_cell(0u, 0u);
  (void)vkr_ui_path(ui, string8_lit("heading"), &arrow, &config);
  /* Margins cannot be negative: a camera off the map's left or top edge
     shows no dot. */
  if (eye.x < 6.0f || eye.y < 6.0f) {
    return;
  }
  VkrUiWidgetConfig dot = vkr_ui_widget_config_default();
  dot.placement = vkr_editor_perf_cell(0u, 0u);
  dot.placement.justify = dot.placement.align = VKR_UI_ALIGN_START;
  dot.placement.margin_pt =
      (VkrUiEdges){eye.y - 6.0f, 0.0f, 0.0f, eye.x - 6.0f};
  dot.style.min_size_pt = dot.style.max_size_pt = (Vec2){12.0f, 12.0f};
  dot.style.background_color = theme->accent;
  dot.style.border_pt = (VkrUiEdges){2.0f, 2.0f, 2.0f, 2.0f};
  dot.style.border_color = theme->text;
  dot.style.corner_radius_pt = (Vec4){6.0f, 6.0f, 6.0f, 6.0f};
  vkr_ui_label(ui, string8_lit("camera"), (String8){0}, &dot);
}

/* Pins at the worst spikes, numbered by rank. */
vkr_internal void draws_spikes(VkrEditorRecordingPerf *perf, VkrUiSystem *ui) {
  const VkrRecordingData *data = &perf->data;
  const VkrEditorPerfCanvas *canvas = &perf->view.draws_canvas;
  const VkrUiTheme *theme = vkr_ui_theme();
  const uint32_t count = Min(perf->analysis.spike_count, 9u);
  for (uint32_t i = 0u; i < count; ++i) {
    const uint64_t f = perf->analysis.spikes[i].frame;
    if (f >= data->frame_count) {
      continue;
    }
    const Vec2 at = draws_point(canvas, (float32_t)data->positions[f * 3u],
                                (float32_t)data->positions[f * 3u + 2u]);
    if (at.x < 8.0f || at.y < 8.0f || at.x > canvas->size_pt.x ||
        at.y > canvas->size_pt.y) {
      continue;
    }
    VkrUiWidgetConfig pin =
        vkr_editor_text_config(theme->font_caption, theme->text);
    pin.placement = vkr_editor_perf_cell(0u, 0u);
    pin.placement.justify = pin.placement.align = VKR_UI_ALIGN_START;
    pin.placement.margin_pt =
        (VkrUiEdges){at.y - 8.0f, 0.0f, 0.0f, at.x - 8.0f};
    pin.style.min_size_pt = pin.style.max_size_pt = (Vec2){16.0f, 16.0f};
    pin.style.background_color = theme->window;
    pin.style.border_pt = (VkrUiEdges){2.0f, 2.0f, 2.0f, 2.0f};
    pin.style.border_color = theme->error;
    pin.style.corner_radius_pt = (Vec4){8.0f, 8.0f, 8.0f, 8.0f};
    pin.center = true_v;
    pin.tooltip = string8_create_formatted(ui->frame_allocator,
                                           "Spike %u  \xc2\xb7  frame %llu",
                                           i + 1u, (unsigned long long)f);
    (void)vkr_ui_push_id_u64(ui, i);
    vkr_ui_label(ui, string8_lit("spike"),
                 string8_create_formatted(ui->frame_allocator, "%u", i + 1u),
                 &pin);
    (void)vkr_ui_pop_id(ui);
  }
}

/* The ground's cells: grid lines and each cell's drawn objects. */
vkr_internal void draws_cells(VkrEditorRecordingPerf *perf, VkrUiSystem *ui) {
  const VkrEditorPerfDraws *draws = &perf->draws;
  const VkrEditorPerfCanvas *canvas = &perf->view.draws_canvas;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrRecordingDrawBounds *set = &perf->data.draw_bounds[draws->bounds];
  const float32_t size =
      set->cell_size >= 1.0f ? set->cell_size : DRAWS_CELL_DEFAULT;
  const int32_t x0 = (int32_t)vkr_floor_f32(draws->extent_min.x / size);
  const int32_t z0 = (int32_t)vkr_floor_f32(draws->extent_min.y / size);
  const int32_t x1 = (int32_t)vkr_floor_f32(draws->extent_max.x / size);
  const int32_t z1 = (int32_t)vkr_floor_f32(draws->extent_max.y / size);
  const uint32_t width = (uint32_t)Min(x1 - x0 + 1, 64);
  const uint32_t depth = (uint32_t)Min(z1 - z0 + 1, 64);
  /* Grid lines, each a stroke between breaks. */
  Vec2 lines[2u * (64u + 1u) * 3u];
  uint32_t count = 0u;
  const float32_t left = (float32_t)x0 * size;
  const float32_t right = (float32_t)(x0 + (int32_t)width) * size;
  const float32_t top = (float32_t)z0 * size;
  const float32_t bottom = (float32_t)(z0 + (int32_t)depth) * size;
  for (uint32_t i = 0u; i <= width; ++i) {
    const float32_t x = left + (float32_t)i * size;
    lines[count++] = draws_point(canvas, x, top);
    lines[count++] = draws_point(canvas, x, bottom);
    lines[count++] = (Vec2){VKR_FLOAT_NAN, VKR_FLOAT_NAN};
  }
  for (uint32_t i = 0u; i <= depth; ++i) {
    const float32_t z = top + (float32_t)i * size;
    lines[count++] = draws_point(canvas, left, z);
    lines[count++] = draws_point(canvas, right, z);
    lines[count++] = (Vec2){VKR_FLOAT_NAN, VKR_FLOAT_NAN};
  }
  const Vec4 palette[] = {vkr_ui_color_alpha(theme->border_strong, 0.9f)};
  const VkrUiPathConfig path = {
      .points = lines,
      .count = count,
      .palette = palette,
      .palette_count = 1u,
      .width_pt = 1.0f,
  };
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement = vkr_editor_perf_cell(0u, 0u);
  (void)vkr_ui_path(ui, string8_lit("cells"), &path, &config);
  /* Drawn objects per cell, by their box centres. */
  uint32_t *drawn =
      vkr_allocator_alloc(ui->frame_allocator, sizeof(uint32_t) * width * depth,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!drawn) {
    return;
  }
  MemZero(drawn, sizeof(uint32_t) * width * depth);
  for (uint32_t o = 0u; o < draws->object_count; ++o) {
    const uint8_t state = draws->object_state[o];
    if (state != VKR_RECORDING_DRAW_STATE_DRAWN &&
        state != VKR_RECORDING_DRAW_STATE_DRAWN_LATE) {
      continue;
    }
    const VkrRecordingDrawObject *object = &set->objects[o];
    const int32_t cx = (int32_t)vkr_floor_f32(
                           (object->min[0] + object->max[0]) * 0.5f / size) -
                       x0;
    const int32_t cz = (int32_t)vkr_floor_f32(
                           (object->min[2] + object->max[2]) * 0.5f / size) -
                       z0;
    if (cx >= 0 && cz >= 0 && (uint32_t)cx < width && (uint32_t)cz < depth) {
      ++drawn[(uint32_t)cz * width + (uint32_t)cx];
    }
  }
  uint32_t labels = 0u;
  for (uint32_t cz = 0u; cz < depth && labels < DRAWS_CELL_LABELS; ++cz) {
    for (uint32_t cx = 0u; cx < width && labels < DRAWS_CELL_LABELS; ++cx) {
      const uint32_t value = drawn[cz * width + cx];
      const Vec2 at = draws_point(canvas, left + (float32_t)cx * size,
                                  top + (float32_t)cz * size);
      if (value == 0u || at.x < 0.0f || at.y < 0.0f ||
          at.x > canvas->size_pt.x - 30.0f ||
          at.y > canvas->size_pt.y - 14.0f || size * canvas->zoom < 46.0f) {
        continue;
      }
      VkrUiWidgetConfig label =
          vkr_editor_text_config(theme->font_caption, theme->text_secondary);
      label.placement = vkr_editor_perf_cell(0u, 0u);
      label.placement.justify = label.placement.align = VKR_UI_ALIGN_START;
      label.placement.margin_pt =
          (VkrUiEdges){at.y + 3.0f, 0.0f, 0.0f, at.x + 4.0f};
      (void)vkr_ui_push_id_u64(ui, cz * 64u + cx);
      vkr_ui_label(ui, string8_lit("cell"),
                   string8_create_formatted(ui->frame_allocator, "%u", value),
                   &label);
      (void)vkr_ui_pop_id(ui);
      ++labels;
    }
  }
}

/* The smallest object whose footprint holds `point`, or UINT32_MAX. */
vkr_internal uint32_t draws_object_at(const VkrEditorRecordingPerf *perf,
                                      Vec2 point, uint32_t layers) {
  const VkrEditorPerfDraws *draws = &perf->draws;
  const VkrRecordingDrawBounds *set = &perf->data.draw_bounds[draws->bounds];
  for (uint32_t k = draws->object_count; k-- > 0u;) {
    const uint32_t o = draws->order[k];
    const VkrRecordingDrawObject *object = &set->objects[o];
    const uint8_t state = draws->object_state[o];
    if (point.x >= object->min[0] && point.x <= object->max[0] &&
        point.y >= object->min[2] && point.y <= object->max[2] &&
        draws_state_shown(layers,
                          state == UINT8_MAX
                              ? (uint8_t)VKR_RECORDING_DRAW_STATE_NOT_CAMERA
                              : state)) {
      return o;
    }
  }
  return UINT32_MAX;
}

/* The frame whose camera stood nearest `point`, within DRAWS_PICK_PT. */
vkr_internal uint64_t draws_frame_at(const VkrEditorRecordingPerf *perf,
                                     Vec2 point) {
  const VkrRecordingData *data = &perf->data;
  const float32_t reach =
      DRAWS_PICK_PT / Max(perf->view.draws_canvas.zoom, 1.0e-3f);
  uint64_t best = UINT64_MAX;
  float32_t best_distance = reach * reach;
  for (uint64_t f = 0u; f < data->frame_count; ++f) {
    const float32_t dx = (float32_t)data->positions[f * 3u] - point.x;
    const float32_t dz = (float32_t)data->positions[f * 3u + 2u] - point.y;
    const float32_t distance = dx * dx + dz * dz;
    if (distance < best_distance) {
      best_distance = distance;
      best = f;
    }
  }
  return best;
}

vkr_internal void draws_map(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                            VkrUiPlacement placement, uint64_t at) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrEditorRecordingPerfView *view = &perf->view;
  VkrEditorPerfCanvas *canvas = &view->draws_canvas;
  const VkrEditorPerfDraws *draws = &perf->draws;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const uint32_t layers = draws_layers(view);
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig area = vkr_editor_perf_grid(placement, &fill, 1u, &fill, 1u);
  area.style.background_color = theme->field;
  area.style.border_pt = (VkrUiEdges){1.0f, 1.0f, 1.0f, 1.0f};
  area.style.border_color = theme->border;
  area.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  area.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("draws.map"), &area)) {
    return;
  }
  /* The tooltip names what is under the pointer as of the last build. */
  String8 tip = (String8){0};
  const VkrEditorPerfCanvasInput input = vkr_editor_perf_canvas_input(
      ui, "draws.canvas", canvas, vkr_editor_perf_cell(0u, 0u), tip);
  if ((!canvas->fitted || canvas->zoom <= 0.0f) && canvas->size_pt.x > 1.0f) {
    vkr_editor_perf_canvas_fit(canvas, draws->extent_min, draws->extent_max,
                               20.0f);
  }
  if (view->draws_follow && canvas->zoom > 0.0f) {
    canvas->center = (Vec2){(float32_t)perf->data.positions[at * 3u],
                            (float32_t)perf->data.positions[at * 3u + 2u]};
  }
  if (canvas->zoom > 0.0f) {
    draws_objects(perf, ui, layers);
    if (layers & DRAWS_LAYER_CELLS) {
      draws_cells(perf, ui);
    }
    if (layers & DRAWS_LAYER_PATH) {
      draws_path(perf, ui);
      draws_spikes(perf, ui);
    }
    draws_frustum(perf, ui, at, layers);
  }
  /* A click on the path picks its nearest frame. */
  if (input.clicked) {
    const uint64_t picked = draws_frame_at(perf, input.pointer);
    if (picked != UINT64_MAX) {
      vkr_editor_perf_select(editor, frame, picked);
    }
  }
  /* What the pointer is over: an object and its state, as a caption in
     the map's corner. */
  if (input.hovered && canvas->zoom > 0.0f) {
    const uint32_t object = draws_object_at(perf, input.pointer, layers);
    if (object != UINT32_MAX) {
      const VkrRecordingDrawObject *row =
          &perf->data.draw_bounds[draws->bounds].objects[object];
      const uint8_t state = draws->object_state[object];
      tip = string8_create_formatted(
          ui->frame_allocator,
          "Object %u  \xc2\xb7  %.1f \xc3\x97 %.1f m, %.1f m tall  \xc2\xb7  "
          "%s",
          row->object_id, row->max[0] - row->min[0], row->max[2] - row->min[2],
          row->max[1] - row->min[1],
          state == UINT8_MAX ? "not in this frame's census"
                             : DRAWS_STATE_NAME[Min(state, (uint8_t)5u)]);
    } else {
      tip = string8_create_formatted(
          ui->frame_allocator,
          "%.1f, %.1f  \xc2\xb7  click the path to pick "
          "its nearest frame",
          input.pointer.x, input.pointer.y);
    }
  }
  VkrUiWidgetConfig caption =
      vkr_editor_text_config(theme->font_caption, theme->text);
  caption.placement = vkr_editor_perf_cell(0u, 0u);
  caption.placement.justify = VKR_UI_ALIGN_START;
  caption.placement.align = VKR_UI_ALIGN_END;
  caption.placement.margin_pt = (VkrUiEdges){0.0f, 0.0f, 8.0f, 8.0f};
  caption.style.background_color = vkr_ui_color_alpha(theme->window, 0.88f);
  caption.style.padding_pt = (VkrUiEdges){3.0f, 8.0f, 3.0f, 8.0f};
  caption.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  vkr_ui_label(ui, string8_lit("draws.under"),
               tip.length ? tip
                          : string8_lit("Wheel zooms, drag pans, a click on "
                                        "the path picks its frame"),
               &caption);
  (void)vkr_ui_panel_end(ui);
}

/* ------------------------------------------------------------------------ */
/* Funnel, sections and chart                                                */

/* One step of the funnel: a name, a count and a bar of it against the
   candidates, with a note under it. */
vkr_internal void draws_step(VkrUiSystem *ui, uint32_t row, const char *name,
                             uint32_t value, uint32_t of, Vec4 color,
                             String8 note) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack columns[] = {fill, auto_track};
  const VkrUiTrack rows[] = {{.value = 16.0f, .unit = VKR_UI_TRACK_PX},
                             {.value = 8.0f, .unit = VKR_UI_TRACK_PX},
                             {.value = 14.0f, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig step =
      vkr_editor_perf_grid(vkr_editor_perf_cell(0u, row), columns,
                           ArrayCount(columns), rows, ArrayCount(rows));
  step.style.gap_pt = 1.0f;
  (void)vkr_ui_push_id_u64(ui, row);
  if (vkr_ui_panel_begin(ui, string8_lit("step"), &step)) {
    VkrUiWidgetConfig label =
        vkr_editor_text_config(theme->font_caption, theme->text);
    label.placement = vkr_editor_perf_cell(0u, 0u);
    label.placement.justify = VKR_UI_ALIGN_START;
    vkr_ui_label(
        ui, string8_lit("name"),
        string8_create_from_cstr((const uint8_t *)name, string_length(name)),
        &label);
    VkrUiWidgetConfig count =
        vkr_editor_text_config(theme->font_caption, theme->text);
    count.placement = vkr_editor_perf_cell(1u, 0u);
    count.placement.justify = VKR_UI_ALIGN_END;
    vkr_ui_label(ui, string8_lit("count"),
                 string8_create_formatted(ui->frame_allocator, "%u", value),
                 &count);
    VkrUiWidgetConfig track = vkr_ui_widget_config_default();
    track.placement = vkr_editor_perf_cell(0u, 1u);
    track.placement.column_span = 2u;
    track.style.background_color = theme->field;
    track.style.corner_radius_pt = (Vec4){3.0f, 3.0f, 3.0f, 3.0f};
    /* An empty label has no height of its own. */
    track.style.min_size_pt.y = 8.0f;
    vkr_ui_label(ui, string8_lit("track"), (String8){0}, &track);
    VkrUiWidgetConfig bar = track;
    bar.placement.justify = VKR_UI_ALIGN_START;
    bar.style.background_color = color;
    bar.style.min_size_pt.x = bar.style.max_size_pt.x =
        of > 0u ? 270.0f * (float32_t)value / (float32_t)of : 0.0f;
    bar.style.min_size_pt.y = bar.style.max_size_pt.y = 8.0f;
    if (value > 0u) {
      vkr_ui_label(ui, string8_lit("bar"), (String8){0}, &bar);
    }
    vkr_editor_perf_caption(ui, "note", note, vkr_editor_perf_cell(0u, 2u),
                            theme->text_secondary);
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_pop_id(ui);
}

vkr_internal void draws_funnel(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame,
                               VkrUiPlacement placement, uint64_t at) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrEditorPerfDraws *draws = &perf->draws;
  const VkrRecordingData *data = &perf->data;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack step = {.value = 44.0f, .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack rows[] = {{.value = 20.0f, .unit = VKR_UI_TRACK_PX},
                             step,
                             step,
                             step,
                             step,
                             step,
                             fill};
  VkrUiPanelConfig panel =
      vkr_editor_perf_grid(placement, &fill, 1u, rows, ArrayCount(rows));
  panel.style.padding_pt = (VkrUiEdges){8.0f, 10.0f, 8.0f, 10.0f};
  panel.style.gap_pt = theme->space_xs;
  panel.style.background_color = theme->raised;
  panel.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  panel.style.border_color = theme->border;
  panel.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  if (!vkr_ui_panel_begin(ui, string8_lit("draws.funnel"), &panel)) {
    return;
  }
  VkrRecordingDrawFrame census;
  const bool8_t has = vkr_recording_data_draw_frame(data, at, &census);
  VkrUiWidgetConfig title =
      vkr_editor_text_config(theme->font_body, theme->text);
  title.placement = vkr_editor_perf_cell(0u, 0u);
  title.placement.justify = VKR_UI_ALIGN_START;
  title.text.font = editor->heading_font;
  vkr_ui_label(ui, string8_lit("title"),
               string8_create_formatted(ui->frame_allocator,
                                        "Camera culling  \xc2\xb7  frame %llu",
                                        (unsigned long long)at),
               &title);
  const uint32_t *counts = draws->state_counts;
  const uint32_t candidates = counts[VKR_RECORDING_DRAW_STATE_DRAWN] +
                              counts[VKR_RECORDING_DRAW_STATE_DRAWN_LATE] +
                              counts[VKR_RECORDING_DRAW_STATE_OCCLUDED] +
                              counts[VKR_RECORDING_DRAW_STATE_FRUSTUM] +
                              counts[VKR_RECORDING_DRAW_STATE_DISTANCE];
  const uint32_t in_view =
      candidates - counts[VKR_RECORDING_DRAW_STATE_FRUSTUM];
  const uint32_t in_range = in_view - counts[VKR_RECORDING_DRAW_STATE_DISTANCE];
  const uint32_t visible = counts[VKR_RECORDING_DRAW_STATE_DRAWN] +
                           counts[VKR_RECORDING_DRAW_STATE_DRAWN_LATE];
  draws_step(ui, 1u, "Candidates", candidates, candidates, theme->text_disabled,
             has ? string8_create_formatted(
                       ui->frame_allocator, "draws the camera tested%s",
                       census.overflow ? "  \xc2\xb7  pool overflowed" : "")
                 : string8_lit("no census at this frame"));
  draws_step(
      ui, 2u, "In the frustum", in_view, candidates,
      DRAWS_STATE_COLOR[DRAWS_COLOR_LATE],
      string8_create_formatted(ui->frame_allocator, "%u outside",
                               counts[VKR_RECORDING_DRAW_STATE_FRUSTUM]));
  draws_step(ui, 3u, "In range", in_range, candidates,
             vkr_ui_color_mix(DRAWS_STATE_COLOR[DRAWS_COLOR_LATE],
                              DRAWS_STATE_COLOR[DRAWS_COLOR_DRAWN], 0.5f),
             string8_create_formatted(
                 ui->frame_allocator, "%u past the draw distance or too small",
                 counts[VKR_RECORDING_DRAW_STATE_DISTANCE]));
  draws_step(ui, 4u, "Visible", visible, candidates,
             DRAWS_STATE_COLOR[DRAWS_COLOR_DRAWN],
             has && (census.flags & VKR_RECORDING_DRAW_STATES_TWO_PHASE)
                 ? string8_create_formatted(
                       ui->frame_allocator,
                       "%u occluded  \xc2\xb7  %u deferred, then drawn late",
                       counts[VKR_RECORDING_DRAW_STATE_OCCLUDED],
                       counts[VKR_RECORDING_DRAW_STATE_DRAWN_LATE])
                 : string8_create_formatted(
                       ui->frame_allocator, "%u occluded by the depth pyramid",
                       counts[VKR_RECORDING_DRAW_STATE_OCCLUDED]));
  const uint32_t calls =
      vkr_recording_data_find_series(data, "draw.world.calls_issued");
  const uint32_t issued = calls != UINT32_MAX && data->series[calls].valid[at]
                              ? (uint32_t)data->series[calls].values[at]
                              : 0u;
  draws_step(ui, 5u, "Draw calls", issued, candidates,
             DRAWS_STATE_COLOR[DRAWS_COLOR_OCCLUDED],
             issued ? string8_lit("issued for every bucket, after batching")
                    : string8_lit("not recorded at this frame"));
  (void)vkr_ui_panel_end(ui);
}

enum {
  SECTION_NAME = 0,
  SECTION_FRAME,
  SECTION_MEDIAN,
  SECTION_COLUMNS,
};

static const VkrEditorTableColumn SECTION_TABLE[SECTION_COLUMNS] = {
    {.title = "Section", .width_pt = 0.0f, .min_pt = 120.0f},
    {.title = "This frame",
     .width_pt = 72.0f,
     .min_pt = 56.0f,
     .numeric = true_v},
    {.title = "vs median",
     .width_pt = 72.0f,
     .min_pt = 56.0f,
     .numeric = true_v,
     .tooltip = "Difference from the median over the shown frames"},
};

vkr_internal int32_t draws_compare_f64(const void *lhs, const void *rhs) {
  const float64_t a = *(const float64_t *)lhs;
  const float64_t b = *(const float64_t *)rhs;
  return a < b ? -1 : (a > b ? 1 : 0);
}

/* Draw counters: every "draw." series at the frame, against its median. */
vkr_internal void draws_counters(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 VkrUiPlacement placement, uint64_t at) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrRecordingData *data = &perf->data;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  uint32_t series[256];
  uint32_t count = 0u;
  for (uint32_t i = 0u; i < data->series_count && count < ArrayCount(series);
       ++i) {
    const char *name = data->series[i].name;
    if (string_length_bounded(name, 5u) == 5u &&
        MemCompare(name, "draw.", 5u) == 0 &&
        perf->analysis.series[i].valid > 0u) {
      series[count++] = i;
    }
  }
  if (count == 0u) {
    vkr_editor_perf_empty(editor, ui, placement, VKR_UI_ICON_DRAWS,
                          "No draw counters",
                          "Tier 1 recordings carry the draws of every pass "
                          "and shadow cascade.");
    return;
  }
  const uint64_t first = perf->view_first;
  const uint64_t last = Min(perf->view_last, data->frame_count - 1u);
  float64_t *values = vkr_allocator_alloc(
      ui->frame_allocator, sizeof(float64_t) * (last + 1u - first),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrEditorTable *table = &perf->view.draws_table;
  bool8_t sorted = false_v;
  uint32_t begin = 0u;
  uint32_t end = 0u;
  if (!values || !vkr_editor_table_begin(ui, "draws.counters", table,
                                         SECTION_TABLE, SECTION_COLUMNS, count,
                                         placement, &sorted, &begin, &end)) {
    return;
  }
  for (uint32_t i = begin; i < end; ++i) {
    const VkrRecordingSeries *s = &data->series[series[i]];
    uint64_t valid = 0u;
    for (uint64_t f = first; f <= last; ++f) {
      if (s->valid[f]) {
        values[valid++] = s->values[f];
      }
    }
    float64_t median = 0.0;
    if (valid > 0u) {
      vkr_sort(values, valid, sizeof(float64_t), draws_compare_f64);
      median = values[(valid - 1u) / 2u];
    }
    const bool8_t here = at < data->frame_count && s->valid[at];
    const float64_t difference = here ? s->values[at] - median : 0.0;
    String8 cells[SECTION_COLUMNS] = {
        string8_create_from_cstr((const uint8_t *)s->name + 5u,
                                 string_length(s->name) - 5u),
        here ? string8_create_formatted(ui->frame_allocator, "%.0f",
                                        s->values[at])
             : string8_lit("\xe2\x80\x94"),
        here
            ? string8_create_formatted(ui->frame_allocator, "%+.0f", difference)
            : (String8){0},
    };
    Vec4 colors[SECTION_COLUMNS] = {0};
    colors[SECTION_MEDIAN] = vkr_abs_f64(difference) <= Max(0.05 * median, 0.5)
                                 ? theme->text_disabled
                             : difference > 0.0 ? theme->warning
                                                : theme->success;
    const VkrEditorTableRow row = {
        .key = series[i],
        .cells = cells,
        .cell_colors = colors,
        .tooltip = string8_create_from_cstr((const uint8_t *)s->name,
                                            string_length(s->name)),
    };
    (void)vkr_editor_table_row(ui, table, SECTION_TABLE, SECTION_COLUMNS, i,
                               &row);
  }
  vkr_editor_table_end(ui);
}

enum {
  CELL_NAME = 0,
  CELL_DRAWN,
  CELL_OCCLUDED,
  CELL_CULLED,
  CELL_COLUMNS,
};

static const VkrEditorTableColumn CELL_TABLE[CELL_COLUMNS] = {
    {.title = "Cell",
     .width_pt = 0.0f,
     .min_pt = 80.0f,
     .tooltip = "A square of the ground, x and z; click to show it"},
    {.title = "Drawn", .width_pt = 58.0f, .min_pt = 46.0f, .numeric = true_v},
    {.title = "Occluded",
     .width_pt = 66.0f,
     .min_pt = 50.0f,
     .numeric = true_v},
    {.title = "Culled",
     .width_pt = 58.0f,
     .min_pt = 46.0f,
     .numeric = true_v,
     .tooltip = "Outside the frustum or past the draw distance"},
};

typedef struct DrawsCell {
  int32_t x;
  int32_t z;
  uint32_t drawn;
  uint32_t occluded;
  uint32_t culled;
} DrawsCell;

vkr_internal int32_t draws_compare_cells(const void *lhs, const void *rhs) {
  const DrawsCell *a = lhs;
  const DrawsCell *b = rhs;
  if (a->drawn != b->drawn) {
    return a->drawn > b->drawn ? -1 : 1;
  }
  if (a->z != b->z) {
    return a->z < b->z ? -1 : 1;
  }
  return a->x < b->x ? -1 : (a->x > b->x ? 1 : 0);
}

/* Objects per cell of the ground by state, the busiest first; a click
   shows the cell on the map with the Cells layer on. */
vkr_internal void draws_cell_table(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrEditorRecordingPerfView *view = &perf->view;
  const VkrEditorPerfDraws *draws = &perf->draws;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrRecordingDrawBounds *set = &perf->data.draw_bounds[draws->bounds];
  const float32_t size =
      set->cell_size >= 1.0f ? set->cell_size : DRAWS_CELL_DEFAULT;
  const int32_t x0 = (int32_t)vkr_floor_f32(draws->extent_min.x / size);
  const int32_t z0 = (int32_t)vkr_floor_f32(draws->extent_min.y / size);
  const uint32_t width = (uint32_t)Min(
      (int32_t)vkr_floor_f32(draws->extent_max.x / size) - x0 + 1, 64);
  const uint32_t depth = (uint32_t)Min(
      (int32_t)vkr_floor_f32(draws->extent_max.y / size) - z0 + 1, 64);
  DrawsCell *cells = vkr_allocator_alloc(ui->frame_allocator,
                                         sizeof(DrawsCell) * width * depth,
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!cells) {
    return;
  }
  for (uint32_t cz = 0u; cz < depth; ++cz) {
    for (uint32_t cx = 0u; cx < width; ++cx) {
      cells[cz * width + cx] =
          (DrawsCell){.x = x0 + (int32_t)cx, .z = z0 + (int32_t)cz};
    }
  }
  for (uint32_t o = 0u; o < draws->object_count; ++o) {
    const uint8_t state = draws->object_state[o];
    if (state == UINT8_MAX || state == VKR_RECORDING_DRAW_STATE_NOT_CAMERA) {
      continue;
    }
    const VkrRecordingDrawObject *object = &set->objects[o];
    const int32_t cx = (int32_t)vkr_floor_f32(
                           (object->min[0] + object->max[0]) * 0.5f / size) -
                       x0;
    const int32_t cz = (int32_t)vkr_floor_f32(
                           (object->min[2] + object->max[2]) * 0.5f / size) -
                       z0;
    if (cx < 0 || cz < 0 || (uint32_t)cx >= width || (uint32_t)cz >= depth) {
      continue;
    }
    DrawsCell *cell = &cells[(uint32_t)cz * width + (uint32_t)cx];
    if (state == VKR_RECORDING_DRAW_STATE_DRAWN ||
        state == VKR_RECORDING_DRAW_STATE_DRAWN_LATE) {
      ++cell->drawn;
    } else if (state == VKR_RECORDING_DRAW_STATE_OCCLUDED) {
      ++cell->occluded;
    } else {
      ++cell->culled;
    }
  }
  /* Only cells with objects, the busiest first. */
  uint32_t count = 0u;
  for (uint32_t i = 0u; i < width * depth; ++i) {
    if (cells[i].drawn + cells[i].occluded + cells[i].culled > 0u) {
      cells[count++] = cells[i];
    }
  }
  vkr_sort(cells, count, sizeof(DrawsCell), draws_compare_cells);
  bool8_t sorted = false_v;
  uint32_t begin = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, "draws.cells", &view->draws_table, CELL_TABLE,
                              CELL_COLUMNS, count, placement, &sorted, &begin,
                              &end)) {
    return;
  }
  for (uint32_t i = begin; i < end; ++i) {
    const DrawsCell *cell = &cells[i];
    String8 values[CELL_COLUMNS] = {
        string8_create_formatted(ui->frame_allocator, "%d, %d", cell->x,
                                 cell->z),
        string8_create_formatted(ui->frame_allocator, "%u", cell->drawn),
        string8_create_formatted(ui->frame_allocator, "%u", cell->occluded),
        string8_create_formatted(ui->frame_allocator, "%u", cell->culled),
    };
    Vec4 colors[CELL_COLUMNS] = {0};
    colors[CELL_CULLED] = theme->text_secondary;
    const VkrEditorTableRow row = {
        .key = ((uint64_t)(uint32_t)cell->z << 32u) | (uint32_t)cell->x,
        .cells = values,
        .cell_colors = colors,
        .tooltip = string8_create_formatted(
            ui->frame_allocator,
            "Cell %d, %d: %.0f to %.0f m in x, %.0f to %.0f m in z", cell->x,
            cell->z, (float64_t)cell->x * size, (float64_t)(cell->x + 1) * size,
            (float64_t)cell->z * size, (float64_t)(cell->z + 1) * size),
    };
    if (vkr_editor_table_row(ui, &view->draws_table, CELL_TABLE, CELL_COLUMNS,
                             i, &row)) {
      view->draws_canvas.center = (Vec2){((float32_t)cell->x + 0.5f) * size,
                                         ((float32_t)cell->z + 0.5f) * size};
      view->draws_follow = false_v;
      view->draws_layers = (draws_layers(view) | DRAWS_LAYER_CELLS) + 1u;
    }
  }
  vkr_editor_table_end(ui);
}

vkr_internal void draws_chart(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame,
                              VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrEditorPerfDraws *draws = &perf->draws;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.value = VKR_EDITOR_PERF_CHART_TITLE_PT, .unit = VKR_UI_TRACK_PX}, fill};
  VkrUiPanelConfig panel =
      vkr_editor_perf_grid(placement, &fill, 1u, rows, ArrayCount(rows));
  panel.style.gap_pt = vkr_ui_theme()->space_xs;
  if (!vkr_ui_panel_begin(ui, string8_lit("draws.chart"), &panel)) {
    return;
  }
  Vec4 colors[VKR_EDITOR_PERF_DRAW_SERIES];
  for (uint32_t s = 0u; s < VKR_EDITOR_PERF_DRAW_SERIES; ++s) {
    colors[s] = DRAWS_STATE_COLOR[1u + s];
  }
  /* The culled states read on the field only lightened. */
  colors[3] = vkr_ui_color_alpha(vkr_ui_theme()->text_secondary, 0.45f);
  colors[4] = vkr_ui_color_alpha(vkr_ui_theme()->text_secondary, 0.25f);
  vkr_editor_perf_chart_title(
      ui, "title", vkr_editor_perf_cell(0u, 0u),
      string8_create_formatted(ui->frame_allocator,
                               "Camera candidates by what culling did, stacked "
                               "\xc2\xb7  top %.0f",
                               draws->top),
      DRAWS_SERIES_LABEL, colors, VKR_EDITOR_PERF_DRAW_SERIES);
  VkrUiPlotSeries bars[VKR_EDITOR_PERF_DRAW_SERIES];
  for (uint32_t s = 0u; s < VKR_EDITOR_PERF_DRAW_SERIES; ++s) {
    bars[s] = (VkrUiPlotSeries){.values = draws->counts[s], .color = colors[s]};
  }
  String8 tip = (String8){0};
  const uint32_t c = perf->hovered_column;
  if (c < perf->column_count) {
    tip = string8_create_formatted(
        ui->frame_allocator,
        "Frame %llu\nDrawn %.0f  \xc2\xb7  late %.0f  \xc2\xb7  occluded "
        "%.0f\nOutside the frustum %.0f  \xc2\xb7  past the distance %.0f",
        (unsigned long long)perf->column_frame[c], draws->counts[0][c],
        draws->counts[1][c], draws->counts[2][c], draws->counts[3][c],
        draws->counts[4][c]);
  }
  VkrUiPlotConfig plot = {
      .sample_count = perf->column_count,
      .y_min = 0.0f,
      .y_max = draws->top,
      .bars = bars,
      .bar_count = VKR_EDITOR_PERF_DRAW_SERIES,
  };
  vkr_editor_perf_plot(editor, frame, "draws.plot",
                       vkr_editor_perf_cell(0u, 1u), &plot, false_v, tip);
  (void)vkr_ui_panel_end(ui);
}

/* ------------------------------------------------------------------------ */
/* Tab                                                                       */

void vkr_editor_recording_perf_draws_build(VkrEditorUi *editor,
                                           const VkrSampleUiFrame *frame,
                                           VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrRecordingData *data = &perf->data;
  if (data->draw_bounds_count == 0u || data->draw_frames == 0u) {
    vkr_editor_perf_empty(
        editor, ui, placement, VKR_UI_ICON_DRAWS, "No draw census recorded",
        "Tier 1 recordings from this build read back what the GPU's camera "
        "culling did with every object: drawn, occluded, outside the frustum "
        "or past the draw distance. Record again to see it on the map.");
    return;
  }
  const uint64_t focus = vkr_editor_perf_focus(perf, frame);
  const uint64_t at = focus < data->frame_count ? focus : perf->view_last;
  draws_update(perf);
  draws_take_frame(perf, at);
  if (!perf->draws.order) {
    return;
  }
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.value = theme->control_height, .unit = VKR_UI_TRACK_PX},
      fill,
      {.value = 136.0f, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig body =
      vkr_editor_perf_grid(placement, &fill, 1u, rows, ArrayCount(rows));
  body.style.gap_pt = theme->space_sm;
  if (!vkr_ui_panel_begin(ui, string8_lit("draws.body"), &body)) {
    return;
  }
  draws_toolbar(editor, frame, vkr_editor_perf_cell(0u, 0u), at);
  const VkrUiTrack columns[] = {fill,
                                {.value = 310.0f, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig split = vkr_editor_perf_grid(
      vkr_editor_perf_cell(0u, 1u), columns, ArrayCount(columns), &fill, 1u);
  split.style.gap_pt = theme->space_md;
  if (vkr_ui_panel_begin(ui, string8_lit("draws.split"), &split)) {
    draws_map(editor, frame, vkr_editor_perf_cell(0u, 0u), at);
    const VkrUiTrack side_rows[] = {
        {.value = 268.0f, .unit = VKR_UI_TRACK_PX},
        {.value = theme->control_height, .unit = VKR_UI_TRACK_PX},
        fill};
    VkrUiPanelConfig side =
        vkr_editor_perf_grid(vkr_editor_perf_cell(1u, 0u), &fill, 1u, side_rows,
                             ArrayCount(side_rows));
    side.style.gap_pt = theme->space_sm;
    if (vkr_ui_panel_begin(ui, string8_lit("draws.side"), &side)) {
      draws_funnel(editor, frame, vkr_editor_perf_cell(0u, 0u), at);
      /* Draws by pass and cascade, or objects by cell of the ground. */
      VkrEditorRecordingPerfView *view = &perf->view;
      const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
      const VkrUiTrack heading[] = {fill, auto_track};
      VkrUiPanelConfig title =
          vkr_editor_perf_grid(vkr_editor_perf_cell(0u, 1u), heading,
                               ArrayCount(heading), &fill, 1u);
      if (vkr_ui_panel_begin(ui, string8_lit("draws.sections"), &title)) {
        vkr_editor_perf_caption(
            ui, "caption",
            view->draws_sections == 1u
                ? string8_lit("Objects by cell at this frame")
                : string8_lit("Draws by pass and cascade at this frame"),
            vkr_editor_perf_cell(0u, 0u), theme->text_secondary);
        static const char *const kinds[] = {"Passes", "Cells"};
        static const char *const tips[] = {
            "The frame's draw counters by pass and shadow cascade",
            "Objects drawn, occluded and culled in each cell of the ground"};
        const uint32_t kind = vkr_editor_perf_segmented(
            ui, "kind", vkr_editor_perf_cell(1u, 0u), kinds, tips, 2u,
            Min(view->draws_sections, 1u));
        if (kind != view->draws_sections) {
          view->draws_sections = kind;
          view->draws_table = (VkrEditorTable){0};
        }
        (void)vkr_ui_panel_end(ui);
      }
      if (view->draws_sections == 1u) {
        draws_cell_table(editor, frame, vkr_editor_perf_cell(0u, 2u));
      } else {
        draws_counters(editor, frame, vkr_editor_perf_cell(0u, 2u), at);
      }
      (void)vkr_ui_panel_end(ui);
    }
    (void)vkr_ui_panel_end(ui);
  }
  draws_chart(editor, frame, vkr_editor_perf_cell(0u, 2u));
  (void)vkr_ui_panel_end(ui);
}
