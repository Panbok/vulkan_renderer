#include "editor_recording.h"

#include "containers/vkr_sort.h"
#include "editor_internal.h"
#include "editor_ui.h"

/* The Performance window's Render graph tab: the recorded graph's passes in
   execution order with their times, as a list or as a flow from each pass
   to the passes that read what it writes, beside the selected pass's
   detail. Times hold still between updates of a chosen period, so they can
   be read while a playback plays; a value that moved flashes. */

static const char *const GRAPH_VALUE_LABEL[VKR_EDITOR_PERF_GRAPH_VALUE_COUNT] =
    {"Mean", "Max", "Frame"};
static const char *const GRAPH_VALUE_TIP[VKR_EDITOR_PERF_GRAPH_VALUE_COUNT] = {
    "Each pass's mean time over the update window",
    "Each pass's slowest time in the update window",
    "Each pass's time in the shown frame",
};

/* A value that moves by more than this share flashes for GRAPH_FLASH_S. */
#define GRAPH_FLASH_SHARE 0.06f
#define GRAPH_FLASH_S 0.6
/* A difference from the median smaller than this share reads as none. */
#define GRAPH_SAME_SHARE 0.08f
/* Frames a frame without GPU pass times looks for the nearest one: one
   tiled sampling period. */
#define GRAPH_REACH 8u
/* Flow node size and spacing, in points at zoom 1. */
#define GRAPH_NODE_W 176.0f
#define GRAPH_NODE_H 40.0f
#define GRAPH_COLUMN_W 200.0f
#define GRAPH_ROW_H 66.0f
/* Curved flow edges at most. A curve is 24 quads of the UI's geometry, a
   straight edge one, and a desktop graph has hundreds of edges: past this,
   only the selected pass's lineage curves, up to this many, and the rest are
   straight, so the wires leave room for the nodes drawn over them. */
#define GRAPH_CURVED_EDGES_MAX 160u
/* Rows of the detail panel's lines at most. */
#define GRAPH_DETAIL_LINES 96u

/* Upstream and downstream of the selected pass, from the dataviz palette's
   violet and orange. */
static const Vec4 GRAPH_UPSTREAM = {0.565f, 0.522f, 0.914f, 1.0f};
static const Vec4 GRAPH_DOWNSTREAM = {0.851f, 0.349f, 0.149f, 1.0f};
/* Pass kinds: graphics, compute, transfer. */
static const Vec4 GRAPH_KIND_COLOR[] = {
    {0.224f, 0.529f, 0.898f, 1.0f},
    {0.098f, 0.620f, 0.439f, 1.0f},
    {0.788f, 0.522f, 0.000f, 1.0f},
};
static const char *const GRAPH_KIND_NAME[] = {"render", "compute", "transfer"};

enum {
  GRAPH_COLUMN_ORDER = 0,
  GRAPH_COLUMN_PASS,
  GRAPH_COLUMN_KIND,
  GRAPH_COLUMN_MS,
  GRAPH_COLUMN_SHARE,
  GRAPH_COLUMN_CPU,
  GRAPH_COLUMN_MEDIAN,
  GRAPH_COLUMN_READS,
  GRAPH_COLUMN_WRITES,
  GRAPH_COLUMN_COUNT,
};

/* ------------------------------------------------------------------------ */
/* Graph facts                                                               */

vkr_internal const VkrRecordingGraph *
graph_shown(const VkrEditorRecordingPerf *perf) {
  const uint32_t index = perf->graph.graph;
  return index < perf->data.graph_count ? &perf->data.graphs[index] : NULL;
}

vkr_internal uint32_t graph_pass_count(const VkrRecordingGraph *graph) {
  return Min(graph->pass_count, (uint32_t)VKR_EDITOR_PERF_GRAPH_PASSES);
}

vkr_internal bool8_t graph_scheduled(const VkrRecordingGraphPass *pass) {
  return pass->order != UINT32_MAX;
}

vkr_internal Vec4 graph_kind_color(uint8_t kind) {
  return GRAPH_KIND_COLOR[Min(kind, (uint8_t)2u)];
}

vkr_internal const char *graph_kind_name(uint8_t kind) {
  return GRAPH_KIND_NAME[Min(kind, (uint8_t)2u)];
}

/* The scheduled pass that last wrote `resource` before execution position
   `order`, or UINT32_MAX. */
vkr_internal uint32_t graph_producer(const VkrRecordingGraph *graph,
                                     uint32_t resource, uint32_t order) {
  uint32_t found = UINT32_MAX;
  uint32_t found_order = 0u;
  for (uint32_t p = 0u; p < graph_pass_count(graph); ++p) {
    const VkrRecordingGraphPass *pass = &graph->passes[p];
    if (!graph_scheduled(pass) || pass->order >= order ||
        (found != UINT32_MAX && pass->order < found_order)) {
      continue;
    }
    for (uint32_t u = 0u; u < pass->use_count; ++u) {
      const VkrRecordingGraphUse *use = &graph->uses[pass->first_use + u];
      if (use->resource == resource &&
          (use->flags & VKR_RECORDING_GRAPH_USE_WRITE)) {
        found = p;
        found_order = pass->order;
        break;
      }
    }
  }
  return found;
}

/* Whether pass `p` reads `resource` (reads include read-only depth). */
vkr_internal bool8_t graph_reads(const VkrRecordingGraph *graph, uint32_t p,
                                 uint32_t resource) {
  const VkrRecordingGraphPass *pass = &graph->passes[p];
  for (uint32_t u = 0u; u < pass->use_count; ++u) {
    const VkrRecordingGraphUse *use = &graph->uses[pass->first_use + u];
    if (use->resource == resource &&
        !(use->flags & VKR_RECORDING_GRAPH_USE_WRITE)) {
      return true_v;
    }
  }
  return false_v;
}

/* Pass `p`'s reads and writes. */
vkr_internal void graph_use_counts(const VkrRecordingGraph *graph, uint32_t p,
                                   uint32_t *reads, uint32_t *writes) {
  const VkrRecordingGraphPass *pass = &graph->passes[p];
  *reads = 0u;
  *writes = 0u;
  for (uint32_t u = 0u; u < pass->use_count; ++u) {
    const VkrRecordingGraphUse *use = &graph->uses[pass->first_use + u];
    if (use->flags & VKR_RECORDING_GRAPH_USE_WRITE) {
      ++*writes;
    } else {
      ++*reads;
    }
  }
}

/* The graph pass index of each recorded pass id, or UINT16_MAX. */
vkr_internal void graph_pass_map(const VkrRecordingData *data,
                                 const VkrRecordingGraph *graph,
                                 uint16_t *map) {
  for (uint32_t i = 0u; i < data->pass_count; ++i) {
    map[i] = UINT16_MAX;
  }
  for (uint32_t p = 0u; p < graph_pass_count(graph); ++p) {
    const uint32_t id = graph->passes[p].pass;
    if (id < data->pass_count) {
      map[id] = (uint16_t)p;
    }
  }
}

/* The pass named `name` in `graph`, or UINT32_MAX. */
vkr_internal uint32_t graph_find(const VkrRecordingGraph *graph,
                                 const char *name) {
  const uint64_t length = string_length(name);
  for (uint32_t p = 0u; length > 0u && p < graph_pass_count(graph); ++p) {
    const String8 text = vkr_recording_graph_text(graph, graph->passes[p].name);
    if (text.length == length && MemCompare(text.str, name, length) == 0) {
      return p;
    }
  }
  return UINT32_MAX;
}

/* ------------------------------------------------------------------------ */
/* Values                                                                    */

/* Whether frame `f` has rows the shown times can use. */
vkr_internal bool8_t graph_timed(const VkrRecordingData *data, uint64_t f,
                                 bool8_t gpu) {
  uint32_t count = 0u;
  (void)vkr_recording_data_pass_rows(data, f, &count);
  return count > 0u &&
         (!gpu || (data->pass_frame_flags[f] & VKR_RECORDING_PASSES_GPU_TIMED));
}

/* Each pass's value over frames [first, last], `value` of them; a window
   without timed frames takes the timed frame nearest `at`. Returns the
   frames used. */
vkr_internal uint32_t graph_values(const VkrRecordingData *data,
                                   const uint16_t *map, uint32_t passes,
                                   bool8_t gpu, uint32_t value, uint64_t at,
                                   uint64_t first, uint64_t last, float32_t *ms,
                                   float32_t *cpu_ms, bool8_t *has) {
  float32_t counts[VKR_EDITOR_PERF_GRAPH_PASSES];
  for (uint32_t p = 0u; p < passes; ++p) {
    ms[p] = 0.0f;
    cpu_ms[p] = 0.0f;
    has[p] = false_v;
    counts[p] = 0.0f;
  }
  if (value == VKR_EDITOR_PERF_GRAPH_FRAME) {
    first = last = at;
  }
  uint32_t used = 0u;
  for (uint32_t pass = 0u; pass < 2u && used == 0u; ++pass) {
    if (pass == 1u) {
      /* Nothing timed in the window: the nearest timed frame. */
      const uint64_t near =
          gpu ? vkr_recording_data_gpu_frame_near(data, at, GRAPH_REACH) : at;
      if (near == UINT64_MAX) {
        break;
      }
      first = last = near;
    }
    for (uint64_t f = first; f <= last; ++f) {
      if (!graph_timed(data, f, gpu)) {
        continue;
      }
      ++used;
      uint32_t count = 0u;
      const VkrRecordingPassRow *rows =
          vkr_recording_data_pass_rows(data, f, &count);
      for (uint32_t r = 0u; r < count; ++r) {
        const uint32_t p =
            rows[r].pass < data->pass_count ? map[rows[r].pass] : UINT16_MAX;
        if (p >= passes ||
            (gpu && !(rows[r].flags & VKR_RECORDING_PASS_GPU_VALID))) {
          continue;
        }
        const float32_t time = gpu ? rows[r].gpu_ms : rows[r].cpu_ms;
        if (value == VKR_EDITOR_PERF_GRAPH_MAX) {
          ms[p] = Max(ms[p], time);
          cpu_ms[p] = Max(cpu_ms[p], rows[r].cpu_ms);
        } else {
          ms[p] += time;
          cpu_ms[p] += rows[r].cpu_ms;
        }
        counts[p] += 1.0f;
        has[p] = true_v;
      }
    }
  }
  if (value != VKR_EDITOR_PERF_GRAPH_MAX) {
    for (uint32_t p = 0u; p < passes; ++p) {
      if (counts[p] > 0.0f) {
        ms[p] /= counts[p];
        cpu_ms[p] /= counts[p];
      }
    }
  }
  return used;
}

vkr_internal int32_t graph_compare_f32(const void *lhs, const void *rhs) {
  const float32_t a = *(const float32_t *)lhs;
  const float32_t b = *(const float32_t *)rhs;
  return a < b ? -1 : (a > b ? 1 : 0);
}

/* Each pass's median time over the shown frames. */
vkr_internal void graph_medians(VkrEditorRecordingPerf *perf, VkrUiSystem *ui,
                                const uint16_t *map, uint32_t passes) {
  VkrEditorPerfGraph *graph = &perf->graph;
  const VkrRecordingData *data = &perf->data;
  const uint64_t first = perf->view_first;
  const uint64_t last = Min(perf->view_last, data->frame_count - 1u);
  if (graph->median_graph == graph->graph && graph->median_first == first &&
      graph->median_last == last) {
    return;
  }
  graph->median_graph = graph->graph;
  graph->median_first = first;
  graph->median_last = last;
  const uint64_t frames = last + 1u - first;
  float32_t *values =
      vkr_allocator_alloc(ui->frame_allocator, sizeof(float32_t) * frames,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  for (uint32_t p = 0u; p < passes; ++p) {
    graph->median[p] = 0.0f;
  }
  if (!values) {
    return;
  }
  /* One pass at a time keeps the scratch to one value per frame. */
  for (uint32_t p = 0u; p < passes; ++p) {
    uint64_t count = 0u;
    for (uint64_t f = first; f <= last; ++f) {
      uint32_t row_count = 0u;
      const VkrRecordingPassRow *rows =
          vkr_recording_data_pass_rows(data, f, &row_count);
      for (uint32_t r = 0u; r < row_count; ++r) {
        if (rows[r].pass >= data->pass_count || map[rows[r].pass] != p) {
          continue;
        }
        if (graph->gpu && !(rows[r].flags & VKR_RECORDING_PASS_GPU_VALID)) {
          break;
        }
        values[count++] = graph->gpu ? rows[r].gpu_ms : rows[r].cpu_ms;
        break;
      }
    }
    if (count > 0u) {
      vkr_sort(values, count, sizeof(float32_t), graph_compare_f32);
      graph->median[p] = values[(count - 1u) / 2u];
    }
  }
}

/* Recomputes the values when the window around `at` stepped or what they
   show changed, and notes the ones that moved. */
vkr_internal void graph_update(VkrEditorRecordingPerf *perf, VkrUiSystem *ui,
                               uint64_t at) {
  VkrEditorPerfGraph *graph = &perf->graph;
  const VkrRecordingData *data = &perf->data;
  const VkrEditorRecordingPerfView *view = &perf->view;
  const uint32_t index = vkr_recording_data_graph_at(data, at);
  /* The window's update period, which the times average over. */
  const uint32_t rate = vkr_editor_perf_update_ms(perf);
  const uint32_t value =
      Min(view->graph_value, (uint32_t)VKR_EDITOR_PERF_GRAPH_VALUE_COUNT - 1u);
  uint64_t first = 0u;
  uint64_t last = 0u;
  const uint64_t step = vkr_editor_perf_window(perf, at, rate, &first, &last);
  /* The frame value follows every frame whatever the period. */
  const uint64_t key = value == VKR_EDITOR_PERF_GRAPH_FRAME ? at : step;
  const bool8_t same_view = graph->valid && graph->graph == index &&
                            graph->rate_ms == rate && graph->value == value;
  if (same_view && graph->step == key) {
    return;
  }
  const VkrRecordingGraph *shape =
      index < data->graph_count ? &data->graphs[index] : NULL;
  if (!shape) {
    graph->valid = false_v;
    return;
  }
  const uint32_t passes = graph_pass_count(shape);
  uint16_t map[VKR_RECORDING_PASS_MAX];
  graph_pass_map(data, shape, map);
  float32_t previous[VKR_EDITOR_PERF_GRAPH_PASSES];
  MemCopy(previous, graph->ms, sizeof(previous));
  graph->graph = index;
  graph->rate_ms = rate;
  graph->value = value;
  graph->step = key;
  graph->first = first;
  graph->last = last;
  graph->gpu = data->pass_gpu_frames > 0u;
  graph->timed_frames =
      graph_values(data, map, passes, graph->gpu, value, at, first, last,
                   graph->ms, graph->cpu_ms, graph->has);
  graph->total_ms = 0.0f;
  const float64_t now = vkr_platform_get_absolute_time();
  for (uint32_t p = 0u; p < passes; ++p) {
    graph->total_ms += graph->has[p] ? graph->ms[p] : 0.0f;
    const float32_t moved = vkr_abs_f32(graph->ms[p] - previous[p]);
    if (same_view && graph->has[p] &&
        moved > GRAPH_FLASH_SHARE * Max(previous[p], 0.05f)) {
      graph->changed_at[p] = now;
    } else if (!same_view) {
      graph->changed_at[p] = 0.0;
    }
  }
  /* The trend: this window and the ones before it, oldest first. */
  float32_t ms[VKR_EDITOR_PERF_GRAPH_PASSES];
  float32_t cpu_ms[VKR_EDITOR_PERF_GRAPH_PASSES];
  bool8_t has[VKR_EDITOR_PERF_GRAPH_PASSES];
  uint64_t end = first;
  for (uint32_t k = 0u; k < VKR_EDITOR_PERF_GRAPH_HISTORY; ++k) {
    const uint32_t slot = VKR_EDITOR_PERF_GRAPH_HISTORY - 1u - k;
    if (k == 0u) {
      for (uint32_t p = 0u; p < passes; ++p) {
        graph->history[p][slot] = graph->has[p] ? graph->ms[p] : 0.0f;
      }
      continue;
    }
    bool8_t any = end > 0u;
    uint64_t a = 0u;
    uint64_t b = 0u;
    if (any) {
      (void)vkr_editor_perf_window(perf, end - 1u, rate, &a, &b);
      end = a;
      (void)graph_values(data, map, passes, graph->gpu, value, b, a, b, ms,
                         cpu_ms, has);
    }
    for (uint32_t p = 0u; p < passes; ++p) {
      graph->history[p][slot] = any && has[p] ? ms[p] : 0.0f;
    }
  }
  graph->valid = true_v;
  graph_medians(perf, ui, map, passes);
}

/* ------------------------------------------------------------------------ */
/* Toolbar                                                                   */

/* The view switch, the update period and value, and the graph's version
   with steps to the frames where it changed. */
vkr_internal void graph_toolbar(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame,
                                VkrUiPlacement placement, uint64_t at) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrEditorRecordingPerfView *view = &perf->view;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack columns[] = {auto_track, auto_track, fill,
                                auto_track, auto_track, auto_track};
  VkrUiPanelConfig row =
      vkr_editor_perf_grid(placement, columns, ArrayCount(columns), &fill, 1u);
  row.style.gap_pt = theme->space_md;
  if (!vkr_ui_panel_begin(ui, string8_lit("graph.toolbar"), &row)) {
    return;
  }
  static const char *const views[] = {"List", "Flow"};
  static const char *const view_tips[] = {
      "Passes in execution order with their times",
      "How each pass flows into the passes that read what it writes"};
  view->graph_flow =
      vkr_editor_perf_segmented(ui, "graph.view", vkr_editor_perf_cell(0u, 0u),
                                views, view_tips, 2u, view->graph_flow) == 1u;
  view->graph_value = vkr_editor_perf_segmented(
      ui, "graph.value", vkr_editor_perf_cell(1u, 0u), GRAPH_VALUE_LABEL,
      GRAPH_VALUE_TIP, VKR_EDITOR_PERF_GRAPH_VALUE_COUNT,
      Min(view->graph_value, (uint32_t)VKR_EDITOR_PERF_GRAPH_VALUE_COUNT - 1u));

  /* The graph's version: which shape of how many, and where it began. */
  const VkrRecordingData *data = &perf->data;
  const uint32_t index = vkr_recording_data_graph_at(data, at);
  if (index < data->graph_count) {
    const VkrRecordingGraph *shape = &data->graphs[index];
    VkrUiWidgetConfig version =
        vkr_editor_text_config(theme->font_caption, theme->text_secondary);
    version.placement = vkr_editor_perf_cell(3u, 0u);
    version.placement.align = VKR_UI_ALIGN_CENTER;
    version.tooltip = string8_lit(
        "The graph's shape changes when passes are added, removed or culled; "
        "each change is a new version");
    vkr_ui_label(ui, string8_lit("graph.version"),
                 string8_create_formatted(
                     ui->frame_allocator,
                     "Graph %u of %u  \xc2\xb7  %u passes  \xc2\xb7  "
                     "from frame %llu  \xc2\xb7  %ux%u",
                     index + 1u, data->graph_count, shape->scheduled_count,
                     (unsigned long long)shape->frame, shape->viewport_width,
                     shape->viewport_height),
                 &version);
    const struct {
      VkrUiIcon icon;
      const char *tip;
      uint32_t target;
    } steps[] = {
        {VKR_UI_ICON_CHEVRON_LEFT,
         "Show the frame where the previous version "
         "began",
         index > 0u ? index - 1u : UINT32_MAX},
        {VKR_UI_ICON_CHEVRON_RIGHT,
         "Show the frame where the next version "
         "began",
         index + 1u < data->graph_count ? index + 1u : UINT32_MAX},
    };
    for (uint32_t i = 0u; i < ArrayCount(steps); ++i) {
      VkrUiWidgetConfig button = vkr_editor_icon_button_config(
          4u + i, 0u, steps[i].icon,
          string8_create_from_cstr((const uint8_t *)steps[i].tip,
                                   string_length(steps[i].tip)));
      button.disabled = steps[i].target == UINT32_MAX;
      (void)vkr_ui_push_id_u64(ui, i);
      if (vkr_ui_button(ui, string8_lit("graph.step"), (String8){0}, &button)) {
        vkr_editor_perf_select(editor, frame,
                               data->graphs[steps[i].target].frame);
      }
      (void)vkr_ui_pop_id(ui);
    }
  }
  (void)vkr_ui_panel_end(ui);
}

/* ------------------------------------------------------------------------ */
/* List                                                                      */

vkr_internal const VkrEditorTableColumn GRAPH_COLUMNS[GRAPH_COLUMN_COUNT] = {
    {.title = "#",
     .width_pt = 34.0f,
     .min_pt = 28.0f,
     .numeric = true_v,
     .tooltip = "Execution order"},
    {.title = "Pass", .width_pt = 0.0f, .min_pt = 120.0f},
    {.title = "Kind", .width_pt = 66.0f, .min_pt = 48.0f},
    {.title = "GPU ms",
     .width_pt = 68.0f,
     .min_pt = 52.0f,
     .numeric = true_v,
     .tooltip = "The pass's GPU time: mean, max or frame value"},
    {.title = "Share",
     .width_pt = 92.0f,
     .min_pt = 60.0f,
     .numeric = true_v,
     .tooltip = "The pass's part of the passes' total"},
    {.title = "CPU ms",
     .width_pt = 62.0f,
     .min_pt = 48.0f,
     .numeric = true_v,
     .tooltip = "CPU time recording the pass"},
    {.title = "vs median",
     .width_pt = 78.0f,
     .min_pt = 60.0f,
     .numeric = true_v,
     .tooltip = "Difference from the pass's median over the shown frames"},
    {.title = "Reads", .width_pt = 50.0f, .min_pt = 40.0f, .numeric = true_v},
    {.title = "Writes", .width_pt = 52.0f, .min_pt = 40.0f, .numeric = true_v},
};

typedef struct GraphSortItem {
  uint32_t pass;
  float64_t key;
} GraphSortItem;

vkr_internal int32_t graph_compare_items(const void *lhs, const void *rhs) {
  const GraphSortItem *a = lhs;
  const GraphSortItem *b = rhs;
  if (a->key != b->key) {
    return a->key < b->key ? -1 : 1;
  }
  return a->pass < b->pass ? -1 : (a->pass > b->pass ? 1 : 0);
}

/* Scheduled passes in execution order, then the unscheduled ones; sorted
   by a clicked column instead when there is one. */
vkr_internal uint32_t graph_rows(const VkrEditorRecordingPerf *perf,
                                 const VkrRecordingGraph *shape,
                                 uint32_t *out) {
  const VkrEditorPerfGraph *graph = &perf->graph;
  const VkrEditorTable *table = &perf->view.graph_table;
  const uint32_t passes = graph_pass_count(shape);
  GraphSortItem items[VKR_EDITOR_PERF_GRAPH_PASSES];
  for (uint32_t p = 0u; p < passes; ++p) {
    const VkrRecordingGraphPass *pass = &shape->passes[p];
    uint32_t reads = 0u;
    uint32_t writes = 0u;
    graph_use_counts(shape, p, &reads, &writes);
    float64_t key =
        graph_scheduled(pass) ? (float64_t)pass->order : 1.0e9 + (float64_t)p;
    switch (table->sort_column) {
    case GRAPH_COLUMN_MS:
    case GRAPH_COLUMN_SHARE:
      key = graph->has[p] ? graph->ms[p] : -1.0;
      break;
    case GRAPH_COLUMN_CPU:
      key = graph->has[p] ? graph->cpu_ms[p] : -1.0;
      break;
    case GRAPH_COLUMN_MEDIAN:
      key = graph->has[p] ? graph->ms[p] - graph->median[p] : -1.0e9;
      break;
    case GRAPH_COLUMN_READS:
      key = reads;
      break;
    case GRAPH_COLUMN_WRITES:
      key = writes;
      break;
    case GRAPH_COLUMN_KIND:
      key = pass->kind * 1.0e4 + (graph_scheduled(pass) ? pass->order : 9999);
      break;
    default:
      break;
    }
    items[p] = (GraphSortItem){.pass = p, .key = key};
  }
  vkr_sort(items, passes, sizeof(GraphSortItem), graph_compare_items);
  const bool8_t descending = table->sort_column != VKR_EDITOR_TABLE_NONE &&
                             table->sort_column != GRAPH_COLUMN_ORDER &&
                             table->sort_descending;
  for (uint32_t i = 0u; i < passes; ++i) {
    out[i] = items[descending ? passes - 1u - i : i].pass;
  }
  return passes;
}

vkr_internal void graph_list(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                             const VkrRecordingGraph *shape,
                             VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrEditorRecordingPerfView *view = &perf->view;
  const VkrEditorPerfGraph *graph = &perf->graph;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrEditorTable *table = &view->graph_table;
  if (!table->initialized) {
    table->sort_column = VKR_EDITOR_TABLE_NONE;
  }
  VkrEditorTableColumn columns[GRAPH_COLUMN_COUNT];
  MemCopy(columns, GRAPH_COLUMNS, sizeof(columns));
  if (!graph->gpu) {
    columns[GRAPH_COLUMN_MS].title = "Pass ms";
    columns[GRAPH_COLUMN_MS].tooltip =
        "The recording has no GPU pass times: the pass's CPU time";
  }
  uint32_t order[VKR_EDITOR_PERF_GRAPH_PASSES];
  const uint32_t count = graph_rows(perf, shape, order);
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, "graph.passes", table, columns,
                              GRAPH_COLUMN_COUNT, count, placement, &sorted,
                              &first, &end)) {
    return;
  }
  const float64_t now = vkr_platform_get_absolute_time();
  const float32_t total = Max(graph->total_ms, 1.0e-3f);
  float32_t largest = 1.0e-3f;
  for (uint32_t p = 0u; p < graph_pass_count(shape); ++p) {
    largest = Max(largest, graph->has[p] ? graph->ms[p] : 0.0f);
  }
  for (uint32_t i = first; i < end; ++i) {
    const uint32_t p = order[i];
    const VkrRecordingGraphPass *pass = &shape->passes[p];
    const bool8_t scheduled = graph_scheduled(pass);
    const bool8_t has = graph->has[p];
    uint32_t reads = 0u;
    uint32_t writes = 0u;
    graph_use_counts(shape, p, &reads, &writes);
    const String8 name = vkr_recording_graph_text(shape, pass->name);
    const float32_t difference = graph->ms[p] - graph->median[p];
    const bool8_t same = vkr_abs_f32(difference) <=
                         Max(GRAPH_SAME_SHARE * graph->median[p], 0.005f);
    String8 cells[GRAPH_COLUMN_COUNT] = {
        scheduled ? string8_create_formatted(ui->frame_allocator, "%u",
                                             pass->order + 1u)
                  : string8_lit("\xe2\x80\x94"),
        name,
        string8_create_from_cstr((const uint8_t *)graph_kind_name(pass->kind),
                                 string_length(graph_kind_name(pass->kind))),
        has ? string8_create_formatted(ui->frame_allocator, "%.3f",
                                       graph->ms[p])
            : string8_lit("\xe2\x80\x94"),
        has ? string8_create_formatted(ui->frame_allocator, "%.1f%%",
                                       100.0f * graph->ms[p] / total)
            : (String8){0},
        has ? string8_create_formatted(ui->frame_allocator, "%.3f",
                                       graph->cpu_ms[p])
            : (String8){0},
        /* A rounding sign would read as a change. */
        has ? string8_create_formatted(
                  ui->frame_allocator, "%+.3f",
                  vkr_abs_f32(difference) < 0.0005f ? 0.0f : difference)
            : (String8){0},
        string8_create_formatted(ui->frame_allocator, "%u", reads),
        string8_create_formatted(ui->frame_allocator, "%u", writes),
    };
    Vec4 colors[GRAPH_COLUMN_COUNT] = {0};
    colors[GRAPH_COLUMN_ORDER] = theme->text_disabled;
    colors[GRAPH_COLUMN_KIND] = theme->text_secondary;
    colors[GRAPH_COLUMN_MEDIAN] = same                ? theme->text_disabled
                                  : difference > 0.0f ? theme->error
                                                      : theme->success;
    Vec4 fills[GRAPH_COLUMN_COUNT] = {0};
    const float64_t since = now - graph->changed_at[p];
    if (graph->changed_at[p] > 0.0 && since < GRAPH_FLASH_S) {
      const float32_t fade = (float32_t)(1.0 - since / GRAPH_FLASH_S);
      fills[GRAPH_COLUMN_MS] = vkr_ui_color_alpha(theme->accent, 0.32f * fade);
    }
    float32_t bars[GRAPH_COLUMN_COUNT];
    Vec4 bar_colors[GRAPH_COLUMN_COUNT] = {0};
    for (uint32_t c = 0u; c < GRAPH_COLUMN_COUNT; ++c) {
      bars[c] = -1.0f;
    }
    if (has) {
      /* The bar measures against the slowest pass, so small ones show. */
      bars[GRAPH_COLUMN_SHARE] = graph->ms[p] / largest;
      bar_colors[GRAPH_COLUMN_SHARE] = graph_kind_color(pass->kind);
    }
    const VkrEditorTableRow row = {
        .key = p,
        .cells = cells,
        .dot = graph_kind_color(pass->kind),
        .has_dot = false_v,
        .text_color = scheduled ? theme->text : theme->text_disabled,
        .tooltip = scheduled ? (String8){0}
                   : (pass->flags & VKR_RECORDING_GRAPH_PASS_DISABLED)
                       ? string8_lit("Disabled: not scheduled this frame")
                       : string8_lit("Culled: nothing read what it writes"),
        .cell_colors = colors,
        .cell_fills = fills,
        .cell_bars = bars,
        .bar_colors = bar_colors,
    };
    const bool8_t selected =
        string_length(view->graph_selected) == name.length &&
        MemCompare(view->graph_selected, name.str, name.length) == 0;
    if (selected) {
      table->selected_key = p;
    }
    if (vkr_editor_table_row(ui, table, columns, GRAPH_COLUMN_COUNT, i, &row)) {
      string_format(view->graph_selected, sizeof(view->graph_selected), "%.*s",
                    (int32_t)name.length, (const char *)name.str);
    }
  }
  vkr_editor_table_end(ui);
}

/* ------------------------------------------------------------------------ */
/* Flow                                                                      */

/* The producer of use `u` of scheduled pass `pass` when that use draws an
   edge: a read of what a pass wrote, the first use of its pair. UINT32_MAX
   otherwise. */
vkr_internal uint32_t graph_edge_producer(const VkrRecordingGraph *shape,
                                          const VkrRecordingGraphPass *pass,
                                          uint32_t u) {
  const VkrRecordingGraphUse *use = &shape->uses[pass->first_use + u];
  if (use->resource == UINT32_MAX ||
      (use->flags & VKR_RECORDING_GRAPH_USE_WRITE)) {
    return UINT32_MAX;
  }
  const uint32_t producer = graph_producer(shape, use->resource, pass->order);
  if (producer >= graph_pass_count(shape)) {
    return UINT32_MAX;
  }
  /* An earlier use of this pair draws the edge. */
  for (uint32_t e = 0u; e < u; ++e) {
    const VkrRecordingGraphUse *before = &shape->uses[pass->first_use + e];
    if (before->resource != UINT32_MAX &&
        !(before->flags & VKR_RECORDING_GRAPH_USE_WRITE) &&
        graph_producer(shape, before->resource, pass->order) == producer) {
      return UINT32_MAX;
    }
  }
  return producer;
}

/* Depth of each scheduled pass: one past the deepest pass whose writes it
   reads; and its place among its depth's passes, in execution order. */
vkr_internal void graph_layout(VkrEditorPerfGraph *graph,
                               const VkrRecordingGraph *shape) {
  if (graph->layout_graph == graph->graph && graph->depth_count > 0u) {
    return;
  }
  graph->layout_graph = graph->graph;
  const uint32_t passes = graph_pass_count(shape);
  uint32_t by_order[VKR_EDITOR_PERF_GRAPH_PASSES];
  uint32_t scheduled = 0u;
  for (uint32_t p = 0u; p < passes; ++p) {
    graph->depth[p] = UINT16_MAX;
    if (graph_scheduled(&shape->passes[p])) {
      by_order[scheduled++] = p;
    }
  }
  /* Passes come in execution order, so each producer has its depth. */
  for (uint32_t i = 1u; i < scheduled; ++i) {
    const uint32_t moved = by_order[i];
    uint32_t j = i;
    while (j > 0u &&
           shape->passes[by_order[j - 1u]].order > shape->passes[moved].order) {
      by_order[j] = by_order[j - 1u];
      --j;
    }
    by_order[j] = moved;
  }
  graph->depth_count = 0u;
  graph->widest = 0u;
  graph->edge_count = 0u;
  MemZero(graph->row_width, sizeof(graph->row_width));
  for (uint32_t i = 0u; i < scheduled; ++i) {
    const uint32_t p = by_order[i];
    const VkrRecordingGraphPass *pass = &shape->passes[p];
    uint32_t depth = 0u;
    for (uint32_t u = 0u; u < pass->use_count; ++u) {
      const VkrRecordingGraphUse *use = &shape->uses[pass->first_use + u];
      if (use->resource == UINT32_MAX ||
          (use->flags & VKR_RECORDING_GRAPH_USE_WRITE)) {
        continue;
      }
      const uint32_t producer =
          graph_producer(shape, use->resource, pass->order);
      if (producer < passes && graph->depth[producer] != UINT16_MAX) {
        depth = Max(depth, (uint32_t)graph->depth[producer] + 1u);
      }
    }
    for (uint32_t u = 0u; u < pass->use_count; ++u) {
      graph->edge_count +=
          graph_edge_producer(shape, pass, u) != UINT32_MAX ? 1u : 0u;
    }
    depth = Min(depth, (uint32_t)VKR_EDITOR_PERF_GRAPH_PASSES - 1u);
    graph->depth[p] = (uint16_t)depth;
    graph->slot[p] = graph->row_width[depth]++;
    graph->depth_count = Max(graph->depth_count, depth + 1u);
    graph->widest = Max(graph->widest, (uint32_t)graph->row_width[depth]);
  }
}

/* Pass `p`'s top-left in flow units: rows by depth, centred. */
vkr_internal Vec2 graph_node_at(const VkrEditorPerfGraph *graph, uint32_t p) {
  const uint32_t depth = graph->depth[p];
  const float32_t width = (float32_t)graph->row_width[depth];
  return (Vec2){((float32_t)graph->slot[p] - (width - 1.0f) * 0.5f) *
                        GRAPH_COLUMN_W -
                    GRAPH_NODE_W * 0.5f,
                (float32_t)depth * GRAPH_ROW_H};
}

/* Upstream (1) and downstream (2) of the selected pass, by pass. */
vkr_internal void graph_lineage(const VkrRecordingGraph *shape,
                                uint32_t selected, uint8_t *marks) {
  const uint32_t passes = graph_pass_count(shape);
  MemZero(marks, passes);
  if (selected >= passes || !graph_scheduled(&shape->passes[selected])) {
    return;
  }
  /* Producers walk back in execution order, consumers forward: a pass
     joins when an edge links it to a marked one. */
  bool8_t grew = true_v;
  while (grew) {
    grew = false_v;
    for (uint32_t p = 0u; p < passes; ++p) {
      const VkrRecordingGraphPass *pass = &shape->passes[p];
      if (!graph_scheduled(pass)) {
        continue;
      }
      for (uint32_t u = 0u; u < pass->use_count; ++u) {
        const VkrRecordingGraphUse *use = &shape->uses[pass->first_use + u];
        if (use->resource == UINT32_MAX ||
            (use->flags & VKR_RECORDING_GRAPH_USE_WRITE)) {
          continue;
        }
        const uint32_t producer =
            graph_producer(shape, use->resource, pass->order);
        if (producer >= passes) {
          continue;
        }
        /* p reads what producer wrote. */
        if ((p == selected || marks[p] == 1u) && producer != selected &&
            marks[producer] != 1u) {
          marks[producer] = 1u;
          grew = true_v;
        }
        if ((producer == selected || marks[producer] == 2u) && p != selected &&
            marks[p] != 2u) {
          marks[p] = 2u;
          grew = true_v;
        }
      }
    }
  }
}

/* The colour of a pass's cost, from the panel to the accent. */
vkr_internal Vec4 graph_cost_color(float32_t share) {
  const VkrUiTheme *theme = vkr_ui_theme();
  return vkr_ui_color_mix(vkr_ui_color_mix(theme->raised, theme->accent, 0.35f),
                          theme->accent_hover, Min(Max(share, 0.0f), 1.0f));
}

vkr_internal void graph_flow(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                             const VkrRecordingGraph *shape,
                             VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrEditorRecordingPerfView *view = &perf->view;
  VkrEditorPerfGraph *graph = &perf->graph;
  VkrEditorPerfCanvas *canvas = &view->graph_canvas;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  graph_layout(graph, shape);
  const uint32_t passes = graph_pass_count(shape);

  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig area = vkr_editor_perf_grid(placement, &fill, 1u, &fill, 1u);
  area.style.background_color = theme->field;
  area.style.border_pt = (VkrUiEdges){1.0f, 1.0f, 1.0f, 1.0f};
  area.style.border_color = theme->border;
  area.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  area.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("graph.flow"), &area)) {
    return;
  }
  /* Fit the whole flow when it is first shown or its shape changed. */
  const Vec2 extent_min = {-(float32_t)graph->widest * GRAPH_COLUMN_W * 0.5f,
                           0.0f};
  const Vec2 extent_max = {(float32_t)graph->widest * GRAPH_COLUMN_W * 0.5f,
                           (float32_t)graph->depth_count * GRAPH_ROW_H};
  const VkrEditorPerfCanvasInput input = vkr_editor_perf_canvas_input(
      ui, "graph.canvas", canvas, vkr_editor_perf_cell(0u, 0u), (String8){0});
  if ((!canvas->fitted || canvas->zoom <= 0.0f) && canvas->size_pt.x > 1.0f) {
    vkr_editor_perf_canvas_fit(canvas, extent_min, extent_max, 24.0f);
    /* A deep graph fits only unreadably small: keep names legible and show
       its first rows; the wheel zooms out to the whole of it. */
    canvas->zoom = vkr_clamp_f32(canvas->zoom, 0.85f, 1.0f);
    canvas->center.y = extent_min.y - 24.0f / canvas->zoom +
                       canvas->size_pt.y * 0.5f / canvas->zoom;
  }
  const float32_t zoom = canvas->zoom > 0.0f ? canvas->zoom : 1.0f;
  const uint32_t selected = graph_find(shape, view->graph_selected);
  uint8_t marks[VKR_EDITOR_PERF_GRAPH_PASSES];
  graph_lineage(shape, selected, marks);

  /* Edges: one per producer and consumer pair, under the nodes. */
  uint32_t curved_edges = 0u;
  for (uint32_t p = 0u; p < passes && canvas->zoom > 0.0f; ++p) {
    const VkrRecordingGraphPass *pass = &shape->passes[p];
    if (!graph_scheduled(pass)) {
      continue;
    }
    for (uint32_t u = 0u; u < pass->use_count; ++u) {
      const uint32_t producer = graph_edge_producer(shape, pass, u);
      if (producer == UINT32_MAX) {
        continue;
      }
      const Vec2 from_node = graph_node_at(graph, producer);
      const Vec2 to_node = graph_node_at(graph, p);
      const Vec2 from = vkr_editor_perf_canvas_point(
          canvas, (Vec2){from_node.x + GRAPH_NODE_W * 0.5f,
                         from_node.y + GRAPH_NODE_H});
      const Vec2 to = vkr_editor_perf_canvas_point(
          canvas, (Vec2){to_node.x + GRAPH_NODE_W * 0.5f, to_node.y});
      const bool8_t upstream =
          (marks[producer] == 1u) && (marks[p] == 1u || p == selected);
      const bool8_t downstream =
          (marks[p] == 2u) && (marks[producer] == 2u || producer == selected);
      const bool8_t curved = curved_edges < GRAPH_CURVED_EDGES_MAX &&
                             (graph->edge_count <= GRAPH_CURVED_EDGES_MAX ||
                              upstream || downstream);
      /* Equal end and control points draw a straight wire. */
      const float32_t bow =
          curved ? Max((to.y - from.y) * 0.5f, 18.0f * zoom) : 0.0f;
      const Vec2 points[4] = {
          from, {from.x, from.y + bow}, {to.x, to.y - bow}, to};
      /* The wire lies inside its points' box; one wholly off the canvas
         is left out. */
      Vec2 low = points[0];
      Vec2 high = points[0];
      for (uint32_t i = 1u; i < ArrayCount(points); ++i) {
        low = (Vec2){Min(low.x, points[i].x), Min(low.y, points[i].y)};
        high = (Vec2){Max(high.x, points[i].x), Max(high.y, points[i].y)};
      }
      if (high.x < 0.0f || low.x > canvas->size_pt.x || high.y < 0.0f ||
          low.y > canvas->size_pt.y) {
        continue;
      }
      curved_edges += curved ? 1u : 0u;
      const VkrRecordingGraphUse *use = &shape->uses[pass->first_use + u];
      const VkrRecordingGraphResource *resource =
          &shape->resources[use->resource];
      /* Wider for larger resources: a few MB reads as a thick wire. */
      const float32_t megabytes = (float32_t)resource->bytes / 1048576.0f;
      const float32_t width =
          (1.2f + Min(vkr_log2_f32(1.0f + megabytes) * 0.45f, 2.6f)) *
          Max(zoom, 0.5f);
      VkrUiWidgetConfig wire = vkr_ui_widget_config_default();
      wire.placement = vkr_editor_perf_cell(0u, 0u);
      wire.placement.justify = wire.placement.align = VKR_UI_ALIGN_START;
      wire.style.min_size_pt = wire.style.max_size_pt = (Vec2){1.0f, 1.0f};
      wire.style.text_color = upstream     ? GRAPH_UPSTREAM
                              : downstream ? GRAPH_DOWNSTREAM
                                           : theme->border_strong;
      (void)vkr_ui_push_id_u64(ui, (uint64_t)p * 4096u + u);
      vkr_ui_bezier(ui, string8_lit("edge"), points, width, &wire);
      (void)vkr_ui_pop_id(ui);
    }
  }

  /* Nodes: the pass name, its kind and its time, a strip coloured by its
     share of the passes' time, and the lineage of the selected one in its
     border. */
  const float32_t total = Max(graph->total_ms, 1.0e-3f);
  const float32_t font = vkr_clamp_f32(theme->font_caption * zoom, 7.0f, 14.0f);
  const bool8_t detailed = zoom >= 0.6f;
  for (uint32_t p = 0u; p < passes && canvas->zoom > 0.0f; ++p) {
    const VkrRecordingGraphPass *pass = &shape->passes[p];
    if (!graph_scheduled(pass)) {
      continue;
    }
    const Vec2 at =
        vkr_editor_perf_canvas_point(canvas, graph_node_at(graph, p));
    const Vec2 size = {GRAPH_NODE_W * zoom, GRAPH_NODE_H * zoom};
    if (at.x + size.x < 0.0f || at.y + size.y < 0.0f ||
        at.x > canvas->size_pt.x || at.y > canvas->size_pt.y) {
      continue;
    }
    const bool8_t is_selected = p == selected;
    const Vec4 ring = is_selected      ? theme->text
                      : marks[p] == 1u ? GRAPH_UPSTREAM
                      : marks[p] == 2u ? GRAPH_DOWNSTREAM
                                       : theme->border;
    const String8 name = vkr_recording_graph_text(shape, pass->name);
    VkrUiWidgetConfig node = vkr_ui_widget_config_default();
    node.placement = vkr_editor_perf_cell(0u, 0u);
    node.placement.justify = node.placement.align = VKR_UI_ALIGN_START;
    /* Margins place from the canvas's corner and cannot be negative: a node
       past the left or top edge keeps only its visible part, and its parts
       past that edge are left out. */
    const Vec2 shown = {Max(at.x, 0.0f), Max(at.y, 0.0f)};
    const Vec2 shown_size = {size.x - (shown.x - at.x),
                             size.y - (shown.y - at.y)};
    if (shown_size.x < 2.0f || shown_size.y < 2.0f) {
      continue;
    }
    node.placement.margin_pt = (VkrUiEdges){shown.y, 0.0f, 0.0f, shown.x};
    node.style.min_size_pt = node.style.max_size_pt = shown_size;
    node.style.background_color = theme->raised;
    node.style.hover_background_color = theme->raised_hover;
    node.style.border_pt = is_selected ? (VkrUiEdges){2.0f, 2.0f, 2.0f, 2.0f}
                                       : (VkrUiEdges){1.0f, 1.0f, 1.0f, 1.0f};
    node.style.border_color = ring;
    node.style.corner_radius_pt = (Vec4){6.0f, 6.0f, 6.0f, 6.0f};
    node.style.padding_pt = (VkrUiEdges){0};
    node.tooltip = string8_create_formatted(
        ui->frame_allocator, "%.*s  \xc2\xb7  %s  \xc2\xb7  %.3f ms %s",
        (int32_t)name.length, (const char *)name.str,
        graph_kind_name(pass->kind), graph->has[p] ? graph->ms[p] : 0.0f,
        graph->gpu ? "GPU" : "CPU");
    (void)vkr_ui_push_id_u64(ui, p);
    if (vkr_ui_button(ui, string8_lit("node"), (String8){0}, &node)) {
      string_format(view->graph_selected, sizeof(view->graph_selected), "%.*s",
                    (int32_t)name.length, (const char *)name.str);
    }
    const float32_t share = graph->has[p] ? graph->ms[p] / total : 0.0f;
    if (at.x >= 0.0f && at.y >= 0.0f) {
      VkrUiWidgetConfig strip = vkr_ui_widget_config_default();
      strip.placement = node.placement;
      strip.placement.margin_pt =
          (VkrUiEdges){at.y + 3.0f * zoom, 0.0f, 0.0f, at.x + 3.0f * zoom};
      strip.style.min_size_pt = strip.style.max_size_pt =
          (Vec2){4.0f * zoom, size.y - 6.0f * zoom};
      strip.style.background_color = graph_cost_color(share * 3.0f);
      strip.style.corner_radius_pt = (Vec4){2.0f, 2.0f, 2.0f, 2.0f};
      vkr_ui_label(ui, string8_lit("strip"), (String8){0}, &strip);
    }
    if (at.x + 12.0f * zoom < 0.0f || at.y < 0.0f) {
      (void)vkr_ui_pop_id(ui);
      continue;
    }
    VkrUiWidgetConfig title = vkr_editor_text_config(font, theme->text);
    title.placement = node.placement;
    title.placement.margin_pt =
        (VkrUiEdges){at.y + (detailed ? 5.0f : 12.0f) * zoom, 0.0f, 0.0f,
                     at.x + 12.0f * zoom};
    title.style.max_size_pt = (Vec2){size.x - 16.0f * zoom, 0.0f};
    title.text.font = editor->heading_font;
    vkr_ui_label(ui, string8_lit("name"), name, &title);
    if (detailed) {
      VkrUiWidgetConfig line = vkr_editor_text_config(
          vkr_clamp_f32(theme->font_caption * 0.92f * zoom, 7.0f, 13.0f),
          theme->text_secondary);
      line.placement = node.placement;
      line.placement.margin_pt =
          (VkrUiEdges){at.y + 22.0f * zoom, 0.0f, 0.0f, at.x + 12.0f * zoom};
      line.style.max_size_pt = (Vec2){size.x - 16.0f * zoom, 0.0f};
      vkr_ui_label(
          ui, string8_lit("line"),
          graph->has[p]
              ? string8_create_formatted(ui->frame_allocator,
                                         "%s  \xc2\xb7  %.3f ms  \xc2\xb7  "
                                         "%.0f%%",
                                         graph_kind_name(pass->kind),
                                         graph->ms[p], 100.0f * share)
              : string8_create_from_cstr(
                    (const uint8_t *)graph_kind_name(pass->kind),
                    string_length(graph_kind_name(pass->kind))),
          &line);
    }
    (void)vkr_ui_pop_id(ui);
  }

  /* The flow's own controls: fit, and the key. */
  VkrUiWidgetConfig fit = vkr_ui_widget_config_default();
  fit.placement = vkr_editor_perf_cell(0u, 0u);
  fit.placement.justify = VKR_UI_ALIGN_END;
  fit.placement.align = VKR_UI_ALIGN_START;
  fit.placement.margin_pt = (VkrUiEdges){8.0f, 8.0f, 0.0f, 0.0f};
  vkr_editor_action_style(&fit, editor->heading_font);
  fit.style.font_size_pt = theme->font_caption;
  fit.icon = VKR_UI_ICON_FRAME;
  fit.icon_size_pt = 12.0f;
  fit.tooltip = string8_lit("Fit the whole flow in view");
  vkr_editor_button_size(&fit);
  if (vkr_ui_button(ui, string8_lit("graph.fit"), string8_lit("Fit"), &fit)) {
    canvas->fitted = false_v;
  }
  VkrUiWidgetConfig key =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  key.placement = vkr_editor_perf_cell(0u, 0u);
  key.placement.justify = VKR_UI_ALIGN_START;
  key.placement.align = VKR_UI_ALIGN_END;
  key.placement.margin_pt = (VkrUiEdges){0.0f, 0.0f, 8.0f, 10.0f};
  const uint32_t hidden = passes - shape->scheduled_count;
  vkr_ui_label(
      ui, string8_lit("graph.key"),
      string8_create_formatted(
          ui->frame_allocator,
          "%u passes, %u links%s%s  \xc2\xb7  wheel zooms, drag pans  "
          "\xc2\xb7  a pass's inputs in violet, what reads it in orange",
          shape->scheduled_count, graph->edge_count,
          hidden ? "  \xc2\xb7  " : "",
          hidden ? (hidden == 1u ? "1 culled pass not shown"
                                 : "culled passes not shown")
                 : ""),
      &key);
  (void)input;
  (void)vkr_ui_panel_end(ui);
}

/* ------------------------------------------------------------------------ */
/* Detail                                                                    */

/* A resource's size: its extent and format, or its bytes for a buffer. */
vkr_internal String8 graph_resource_size(VkrUiSystem *ui,
                                         const VkrRecordingGraph *shape,
                                         const VkrRecordingGraphResource *r) {
  const float64_t megabytes = (float64_t)r->bytes / 1048576.0;
  if (r->flags & VKR_RECORDING_GRAPH_RESOURCE_BUFFER) {
    return string8_create_formatted(ui->frame_allocator, "buffer %.2f MB",
                                    megabytes);
  }
  const String8 format = vkr_recording_graph_text(shape, r->format);
  return string8_create_formatted(ui->frame_allocator, "%ux%u%s%.*s  %.2f MB",
                                  r->width, r->height, format.length ? " " : "",
                                  (int32_t)format.length,
                                  (const char *)format.str, megabytes);
}

/* One line of the detail panel: a caption, or a key and a value. */
typedef struct GraphLine {
  String8 key;
  String8 value;
  Vec4 color;
  bool8_t section;
} GraphLine;

vkr_internal void graph_detail(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame,
                               const VkrRecordingGraph *shape,
                               VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrEditorPerfGraph *graph = &perf->graph;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.value = theme->control_height, .unit = VKR_UI_TRACK_PX},
      {.value = 56.0f, .unit = VKR_UI_TRACK_PX},
      fill};
  VkrUiPanelConfig panel =
      vkr_editor_perf_grid(placement, &fill, 1u, rows, ArrayCount(rows));
  panel.style.padding_pt = (VkrUiEdges){8.0f, 10.0f, 8.0f, 10.0f};
  panel.style.gap_pt = theme->space_sm;
  panel.style.background_color = theme->raised;
  panel.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  panel.style.border_color = theme->border;
  panel.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  if (!vkr_ui_panel_begin(ui, string8_lit("graph.detail"), &panel)) {
    return;
  }
  const uint32_t p = graph_find(shape, perf->view.graph_selected);
  if (p == UINT32_MAX) {
    VkrUiWidgetConfig hint =
        vkr_editor_text_config(theme->font_body, theme->text_secondary);
    hint.placement = vkr_editor_perf_cell(0u, 0u);
    hint.placement.row_span = 3u;
    hint.placement.justify = hint.placement.align = VKR_UI_ALIGN_CENTER;
    hint.text.layout.word_wrap = true_v;
    hint.text.layout.max_width = 230.0f;
    hint.text.layout.anchor.horizontal = VKR_TEXT_ALIGN_CENTER;
    vkr_ui_label(ui, string8_lit("hint"),
                 string8_lit("Select a pass to see its trend, what it reads "
                             "and writes, and which passes it feeds."),
                 &hint);
    (void)vkr_ui_panel_end(ui);
    return;
  }
  const VkrRecordingGraphPass *pass = &shape->passes[p];
  const String8 name = vkr_recording_graph_text(shape, pass->name);
  VkrUiWidgetConfig title =
      vkr_editor_text_config(theme->font_body, theme->text);
  title.placement = vkr_editor_perf_cell(0u, 0u);
  title.placement.justify = VKR_UI_ALIGN_START;
  title.placement.align = VKR_UI_ALIGN_CENTER;
  title.text.font = editor->heading_font;
  title.icon = VKR_UI_ICON_DOT;
  title.icon_size_pt = 10.0f;
  title.icon_color = graph_kind_color(pass->kind);
  vkr_ui_label(ui, string8_lit("title"), name, &title);

  /* The trend: the value at each of the last updates, this one on the
     right. */
  float32_t top = 0.05f;
  for (uint32_t k = 0u; k < VKR_EDITOR_PERF_GRAPH_HISTORY; ++k) {
    top = Max(top, graph->history[p][k]);
  }
  const VkrUiPlotSeries bars = {.values = graph->history[p],
                                .color = graph_kind_color(pass->kind)};
  const VkrUiPlotReference median = {
      .value = graph->median[p],
      .color = vkr_ui_color_alpha(theme->text_secondary, 0.8f),
      .dashed = true_v};
  const VkrUiPlotConfig plot = {
      .sample_count = VKR_EDITOR_PERF_GRAPH_HISTORY,
      .y_min = 0.0f,
      .y_max = top * 1.1f,
      .bars = &bars,
      .bar_count = 1u,
      .references = &median,
      .reference_count = 1u,
      .cursor = VKR_EDITOR_PERF_GRAPH_HISTORY - 1u,
      .cursor_color = theme->accent,
      .range_first = VKR_UI_PLOT_NONE,
      .range_last = VKR_UI_PLOT_NONE,
  };
  VkrUiWidgetConfig trend = vkr_ui_widget_config_default();
  trend.placement = vkr_editor_perf_cell(0u, 1u);
  trend.style.background_color = theme->field;
  trend.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  trend.style.padding_pt = (VkrUiEdges){4.0f, 4.0f, 4.0f, 4.0f};
  trend.tooltip = string8_create_formatted(
      ui->frame_allocator,
      "%.*s over the last %u updates, the newest on the right; dashed at its "
      "median %.3f ms",
      (int32_t)name.length, (const char *)name.str,
      VKR_EDITOR_PERF_GRAPH_HISTORY, graph->median[p]);
  (void)vkr_ui_plot(ui, string8_lit("trend"), &plot, &trend);

  GraphLine *lines = vkr_allocator_alloc(ui->frame_allocator,
                                         sizeof(GraphLine) * GRAPH_DETAIL_LINES,
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!lines) {
    (void)vkr_ui_panel_end(ui);
    return;
  }
  uint32_t count = 0u;
  const VkrRecordingPassStats *stats = NULL;
  for (uint32_t i = 0u; i < perf->analysis.pass_count; ++i) {
    if (perf->analysis.passes[i].pass == pass->pass) {
      stats = &perf->analysis.passes[i];
    }
  }
  lines[count++] = (GraphLine){
      .key = string8_lit("Kind"),
      .value = string8_create_formatted(
          ui->frame_allocator, "%s  \xc2\xb7  %s", graph_kind_name(pass->kind),
          graph_scheduled(pass)
              ? (const char *)string8_create_formatted(
                    ui->frame_allocator, "#%u in order", pass->order + 1u)
                    .str
              : "not scheduled")};
  lines[count++] = (GraphLine){
      .key = string8_lit(graph->gpu ? "GPU now" : "Time now"),
      .value = graph->has[p] ? string8_create_formatted(ui->frame_allocator,
                                                        "%.3f ms", graph->ms[p])
                             : string8_lit("not timed")};
  if (stats) {
    const VkrRecordingStats *time = graph->gpu ? &stats->gpu : &stats->cpu;
    lines[count++] =
        (GraphLine){.key = string8_lit("Recording"),
                    .value = string8_create_formatted(
                        ui->frame_allocator,
                        "p50 %.3f  \xc2\xb7  p95 %.3f  \xc2\xb7  max %.3f",
                        time->p50, time->p95, time->max)};
    lines[count++] =
        (GraphLine){.key = string8_lit("CPU record"),
                    .value = string8_create_formatted(
                        ui->frame_allocator, "p50 %.3f  \xc2\xb7  max %.3f ms",
                        stats->cpu.p50, stats->cpu.max)};
  }
  lines[count++] = (GraphLine){.key = string8_lit("Reads"), .section = true_v};
  for (uint32_t u = 0u; u < pass->use_count && count + 2u < GRAPH_DETAIL_LINES;
       ++u) {
    const VkrRecordingGraphUse *use = &shape->uses[pass->first_use + u];
    if (use->resource == UINT32_MAX ||
        (use->flags & VKR_RECORDING_GRAPH_USE_WRITE)) {
      continue;
    }
    const VkrRecordingGraphResource *resource =
        &shape->resources[use->resource];
    const uint32_t producer =
        graph_scheduled(pass)
            ? graph_producer(shape, use->resource, pass->order)
            : UINT32_MAX;
    const String8 from =
        producer != UINT32_MAX
            ? vkr_recording_graph_text(shape, shape->passes[producer].name)
            : string8_lit("outside the graph");
    lines[count++] = (GraphLine){
        .key = vkr_recording_graph_text(shape, resource->name),
        .value = string8_create_formatted(
            ui->frame_allocator, "%.*s  \xe2\x86\x90 %.*s",
            (int32_t)graph_resource_size(ui, shape, resource).length,
            (const char *)graph_resource_size(ui, shape, resource).str,
            (int32_t)from.length, (const char *)from.str),
        .color = GRAPH_UPSTREAM};
  }
  lines[count++] = (GraphLine){.key = string8_lit("Writes"), .section = true_v};
  for (uint32_t u = 0u; u < pass->use_count && count + 1u < GRAPH_DETAIL_LINES;
       ++u) {
    const VkrRecordingGraphUse *use = &shape->uses[pass->first_use + u];
    if (use->resource == UINT32_MAX ||
        !(use->flags & VKR_RECORDING_GRAPH_USE_WRITE)) {
      continue;
    }
    const VkrRecordingGraphResource *resource =
        &shape->resources[use->resource];
    /* The passes that read this write before another pass writes it. */
    uint32_t readers = 0u;
    for (uint32_t q = 0u; graph_scheduled(pass) && q < graph_pass_count(shape);
         ++q) {
      const VkrRecordingGraphPass *other = &shape->passes[q];
      if (graph_scheduled(other) && other->order > pass->order &&
          graph_reads(shape, q, use->resource) &&
          graph_producer(shape, use->resource, other->order) == p) {
        ++readers;
      }
    }
    lines[count++] = (GraphLine){
        .key = vkr_recording_graph_text(shape, resource->name),
        .value = string8_create_formatted(
            ui->frame_allocator, "%.*s  \xe2\x86\x92 %u pass%s",
            (int32_t)graph_resource_size(ui, shape, resource).length,
            (const char *)graph_resource_size(ui, shape, resource).str, readers,
            readers == 1u ? "" : "es"),
        .color = GRAPH_DOWNSTREAM};
  }
  VkrUiTrack line_rows[GRAPH_DETAIL_LINES];
  for (uint32_t i = 0u; i < count; ++i) {
    line_rows[i] = (VkrUiTrack){.value = lines[i].section ? 24.0f : 30.0f,
                                .unit = VKR_UI_TRACK_PX};
    if (!lines[i].section && lines[i].color.w <= 0.0f) {
      line_rows[i].value = 18.0f;
    }
  }
  VkrUiPanelConfig list = vkr_editor_perf_grid(vkr_editor_perf_cell(0u, 2u),
                                               &fill, 1u, line_rows, count);
  if (count > 0u && vkr_ui_scroll_area_begin(ui, string8_lit("lines"), &list)) {
    for (uint32_t i = 0u; i < count; ++i) {
      const GraphLine *line = &lines[i];
      (void)vkr_ui_push_id_u64(ui, i);
      if (line->section) {
        VkrUiWidgetConfig section =
            vkr_editor_section_label_config(editor->heading_font);
        section.placement = vkr_editor_perf_cell(0u, i);
        section.placement.align = VKR_UI_ALIGN_END;
        vkr_ui_label(ui, string8_lit("section"), line->key, &section);
      } else if (line->color.w > 0.0f) {
        /* A resource: its name over its size and where it goes. */
        VkrUiWidgetConfig resource =
            vkr_editor_text_config(theme->font_caption, theme->text);
        resource.placement = vkr_editor_perf_cell(0u, i);
        resource.placement.justify = VKR_UI_ALIGN_START;
        resource.placement.align = VKR_UI_ALIGN_START;
        resource.icon = VKR_UI_ICON_DOT;
        resource.icon_size_pt = 8.0f;
        resource.icon_color = line->color;
        vkr_ui_label(ui, string8_lit("resource"), line->key, &resource);
        VkrUiWidgetConfig where =
            vkr_editor_text_config(theme->font_caption, theme->text_secondary);
        where.placement = vkr_editor_perf_cell(0u, i);
        where.placement.justify = VKR_UI_ALIGN_START;
        where.placement.align = VKR_UI_ALIGN_END;
        where.placement.margin_pt.left = 14.0f;
        vkr_ui_label(ui, string8_lit("where"), line->value, &where);
      } else {
        VkrUiWidgetConfig key =
            vkr_editor_text_config(theme->font_caption, theme->text_secondary);
        key.placement = vkr_editor_perf_cell(0u, i);
        key.placement.justify = VKR_UI_ALIGN_START;
        key.placement.align = VKR_UI_ALIGN_CENTER;
        vkr_ui_label(ui, string8_lit("key"), line->key, &key);
        VkrUiWidgetConfig value =
            vkr_editor_text_config(theme->font_caption, theme->text);
        value.placement = vkr_editor_perf_cell(0u, i);
        value.placement.justify = VKR_UI_ALIGN_START;
        value.placement.align = VKR_UI_ALIGN_CENTER;
        value.placement.margin_pt.left = 78.0f;
        vkr_ui_label(ui, string8_lit("value"), line->value, &value);
      }
      (void)vkr_ui_pop_id(ui);
    }
    (void)vkr_ui_scroll_area_end(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

/* ------------------------------------------------------------------------ */
/* Tab                                                                       */

void vkr_editor_recording_perf_graph_build(VkrEditorUi *editor,
                                           const VkrSampleUiFrame *frame,
                                           VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrRecordingData *data = &perf->data;
  if (data->graph_count == 0u) {
    vkr_editor_perf_empty(
        editor, ui, placement, VKR_UI_ICON_GRAPH, "No render graph recorded",
        data->pass_frames > 0u
            ? "This recording has pass times but not the graph's shape, which "
              "recordings from this build onward hold. Record again with Tier "
              "1 metrics (the default) to see each pass, its links and its "
              "trend."
            : "Recordings with Tier 1 metrics (the default) hold the render "
              "graph's passes, how they connect and their times. This one was "
              "made without them.");
    return;
  }
  const uint64_t focus = vkr_editor_perf_focus(perf, frame);
  const uint64_t at = focus < data->frame_count ? focus : data->graphs[0].frame;
  graph_update(perf, ui, Min(at, data->frame_count - 1u));
  const VkrRecordingGraph *shape = graph_shown(perf);
  if (!shape) {
    return;
  }
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.value = theme->control_height + 2.0f, .unit = VKR_UI_TRACK_PX},
      fill,
      {.value = 16.0f, .unit = VKR_UI_TRACK_PX},
  };
  VkrUiPanelConfig body =
      vkr_editor_perf_grid(placement, &fill, 1u, rows, ArrayCount(rows));
  body.style.gap_pt = theme->space_sm;
  if (!vkr_ui_panel_begin(ui, string8_lit("graph.body"), &body)) {
    return;
  }
  graph_toolbar(editor, frame, vkr_editor_perf_cell(0u, 0u), at);
  const VkrUiTrack columns[] = {fill,
                                {.value = 300.0f, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig split = vkr_editor_perf_grid(
      vkr_editor_perf_cell(0u, 1u), columns, ArrayCount(columns), &fill, 1u);
  split.style.gap_pt = theme->space_md;
  if (vkr_ui_panel_begin(ui, string8_lit("graph.split"), &split)) {
    if (perf->view.graph_flow) {
      graph_flow(editor, frame, shape, vkr_editor_perf_cell(0u, 0u));
    } else {
      graph_list(editor, frame, shape, vkr_editor_perf_cell(0u, 0u));
    }
    graph_detail(editor, frame, shape, vkr_editor_perf_cell(1u, 0u));
    (void)vkr_ui_panel_end(ui);
  }
  /* What the numbers are: the window they cover and its timed frames. */
  const VkrEditorPerfGraph *graph = &perf->graph;
  const char *what = graph->value == VKR_EDITOR_PERF_GRAPH_MAX     ? "slowest"
                     : graph->value == VKR_EDITOR_PERF_GRAPH_FRAME ? "frame"
                                                                   : "mean";
  vkr_editor_perf_caption(
      ui, "graph.window",
      graph->rate_ms == 0u || graph->value == VKR_EDITOR_PERF_GRAPH_FRAME
          ? string8_create_formatted(
                ui->frame_allocator,
                "%s %s times of frame %llu%s  \xc2\xb7  passes total %.3f ms",
                what, graph->gpu ? "GPU" : "CPU", (unsigned long long)at,
                graph->timed_frames == 0u ? " (not timed)" : "",
                graph->total_ms)
          : string8_create_formatted(
                ui->frame_allocator,
                "%s %s times over frames %llu to %llu, updated every %u ms of "
                "the recording  \xc2\xb7  %u timed frame%s  \xc2\xb7  passes "
                "total %.3f ms",
                what, graph->gpu ? "GPU" : "CPU",
                (unsigned long long)graph->first,
                (unsigned long long)graph->last, graph->rate_ms,
                graph->timed_frames, graph->timed_frames == 1u ? "" : "s",
                graph->total_ms),
      vkr_editor_perf_cell(0u, 2u), theme->text_secondary);
  (void)vkr_ui_panel_end(ui);
}
