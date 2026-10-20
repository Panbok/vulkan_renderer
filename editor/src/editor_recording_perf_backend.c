#include "editor_recording.h"

#include "containers/vkr_sort.h"
#include "editor_internal.h"
#include "editor_ui.h"

/* The Performance window's Backend tab, named after the backend that made
   the recording (ADR-087: Metal runs the tiled pipeline, Vulkan the
   desktop one): the shown frame's passes in submission order with their GPU
   and recording times, the CPU cost of building and submitting each frame,
   every backend counter the recording measured, and the CPU functions that
   ran, from its zones. */

/* CPU stages of recording a frame: graph build and compile, packing the
   draw candidates, submission, presentation and the drawable wait. */
static const char *const BACKEND_STAGE_SERIES[VKR_EDITOR_PERF_RECORD_STAGES] = {
    "cpu.graph_build",   "cpu.graph_compile",   "cpu.packet_candidate_pack",
    "cpu.render_submit", "cpu.backend_present", "cpu.drawable_acquire",
};
static const char *const BACKEND_STAGE_LABEL[VKR_EDITOR_PERF_RECORD_STAGES] = {
    "Graph build", "Compile", "Candidates", "Submit", "Present", "Acquire",
};
static const Vec4 BACKEND_STAGE_COLOR[VKR_EDITOR_PERF_RECORD_STAGES] = {
    {0.224f, 0.529f, 0.898f, 1.0f}, {0.851f, 0.349f, 0.149f, 1.0f},
    {0.098f, 0.620f, 0.439f, 1.0f}, {0.788f, 0.522f, 0.000f, 1.0f},
    {0.835f, 0.318f, 0.506f, 1.0f}, {0.565f, 0.522f, 0.914f, 1.0f},
};
/* Pass kinds: graphics, compute, transfer; and a pass without a graph. */
static const Vec4 BACKEND_KIND_COLOR[] = {
    {0.224f, 0.529f, 0.898f, 1.0f},
    {0.098f, 0.620f, 0.439f, 1.0f},
    {0.788f, 0.522f, 0.000f, 1.0f},
    {0.500f, 0.500f, 0.520f, 1.0f},
};
/* A pass the shown graph version does not hold has no kind. */
static const char *const BACKEND_KIND_NAME[] = {"render", "compute", "transfer",
                                                "\xe2\x80\x94"};
#define BACKEND_KIND_UNKNOWN 3u

/* Series prefixes the counter table lists: backend counters first. */
static const char *const BACKEND_COUNTER_PREFIXES[] = {
    "backend.",     "pipeline.", "draw.world.",
    "gpu.present.", "upload.",   "frame.command_slot",
};

/* Frames a frame without GPU pass times looks for the nearest one. */
#define BACKEND_REACH 8u
/* Rows of the counter and function tables at most. */
#define BACKEND_COUNTERS_MAX 256u
#define BACKEND_FUNCTIONS_MAX 1024u

const char *vkr_editor_perf_backend_name(const VkrEditorRecordingPerf *perf) {
  if (string_equals(perf->data.pipeline, "tiled")) {
    return "Metal";
  }
  if (string_equals(perf->data.pipeline, "desktop")) {
    return "Vulkan";
  }
  return "Backend";
}

/* ------------------------------------------------------------------------ */
/* Data                                                                      */

vkr_internal void backend_update(VkrEditorRecordingPerf *perf) {
  VkrEditorPerfBackend *backend = &perf->backend;
  if (backend->valid && backend->columns_version == perf->columns_version) {
    return;
  }
  const VkrRecordingData *data = &perf->data;
  MemZero(backend, sizeof(*backend));
  backend->columns_version = perf->columns_version;
  backend->valid = true_v;
  for (uint32_t s = 0u; s < VKR_EDITOR_PERF_RECORD_STAGES; ++s) {
    backend->stage_series[s] =
        vkr_recording_data_find_series(data, BACKEND_STAGE_SERIES[s]);
    backend->has_stages =
        backend->has_stages || backend->stage_series[s] != UINT32_MAX;
  }
  for (uint32_t c = 0u; c < perf->column_count; ++c) {
    const uint64_t f = perf->column_frame[c];
    float32_t total = 0.0f;
    for (uint32_t s = 0u; s < VKR_EDITOR_PERF_RECORD_STAGES; ++s) {
      const uint32_t series = backend->stage_series[s];
      if (series != UINT32_MAX && data->series[series].valid[f]) {
        backend->stage_ms[s][c] = (float32_t)data->series[series].values[f];
        total += backend->stage_ms[s][c];
      }
    }
    backend->stage_top = Max(backend->stage_top, total);
  }
  backend->stage_top = Max(backend->stage_top * 1.08f, 0.5f);
}

/* The frame the pass views read: `at`, or the nearest with GPU times. */
vkr_internal uint64_t backend_pass_frame(const VkrRecordingData *data,
                                         uint64_t at) {
  if (data->pass_gpu_frames == 0u) {
    return at;
  }
  const uint64_t near =
      vkr_recording_data_gpu_frame_near(data, at, BACKEND_REACH);
  return near != UINT64_MAX ? near : at;
}

/* Pass `pass`'s kind in the graph in effect at `frame`. */
vkr_internal uint8_t backend_kind(const VkrRecordingData *data, uint64_t frame,
                                  uint32_t pass) {
  const uint32_t index = vkr_recording_data_graph_at(data, frame);
  if (index == UINT32_MAX) {
    return BACKEND_KIND_UNKNOWN;
  }
  const VkrRecordingGraph *graph = &data->graphs[index];
  for (uint32_t p = 0u; p < graph->pass_count; ++p) {
    if (graph->passes[p].pass == pass) {
      return Min(graph->passes[p].kind, (uint8_t)2u);
    }
  }
  return BACKEND_KIND_UNKNOWN;
}

/* ------------------------------------------------------------------------ */
/* Cards                                                                     */

/* The mean of series `name` over the shown frames; false without samples. */
vkr_internal bool8_t backend_mean(const VkrEditorRecordingPerf *perf,
                                  const char *name, float64_t *mean,
                                  float64_t *max) {
  const VkrRecordingData *data = &perf->data;
  const uint32_t series = vkr_recording_data_find_series(data, name);
  *mean = 0.0;
  *max = 0.0;
  if (series == UINT32_MAX) {
    return false_v;
  }
  uint64_t count = 0u;
  const uint64_t last = Min(perf->view_last, data->frame_count - 1u);
  for (uint64_t f = perf->view_first; f <= last; ++f) {
    if (data->series[series].valid[f]) {
      *mean += data->series[series].values[f];
      *max = Max(*max, data->series[series].values[f]);
      ++count;
    }
  }
  if (count == 0u) {
    return false_v;
  }
  *mean /= (float64_t)count;
  return true_v;
}

/* A card of series `name`'s mean, with its max on the last line. */
vkr_internal void backend_card(VkrEditorUi *editor, VkrUiSystem *ui,
                               uint32_t column, const char *caption,
                               const char *name, const char *unit,
                               const char *fallback_line) {
  const VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrUiTheme *theme = vkr_ui_theme();
  float64_t mean = 0.0;
  float64_t max = 0.0;
  const bool8_t has = backend_mean(perf, name, &mean, &max);
  const bool8_t ms = string_equals(unit, "ms");
  vkr_editor_perf_card(
      editor, ui, column, caption,
      has ? string8_create_formatted(
                ui->frame_allocator, ms ? "%.2f %s" : "%.0f%s%s", mean,
                ms ? unit : (unit[0] ? " " : ""), ms ? "" : unit)
          : string8_lit("not recorded"),
      has ? theme->text : theme->text_disabled,
      has ? string8_create_formatted(ui->frame_allocator,
                                     ms ? "mean  \xc2\xb7  max %.2f"
                                        : "mean  \xc2\xb7  max %.0f",
                                     max)
          : string8_create_from_cstr((const uint8_t *)fallback_line,
                                     string_length(fallback_line)),
      theme->text_secondary);
}

vkr_internal void backend_cards(VkrEditorUi *editor, VkrUiSystem *ui,
                                VkrUiPlacement placement) {
  const VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack card = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack columns[] = {card, card, card, card, card, card, card};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig row =
      vkr_editor_perf_grid(placement, columns, ArrayCount(columns), &fill, 1u);
  row.style.gap_pt = theme->space_sm;
  if (!vkr_ui_panel_begin(ui, string8_lit("backend.cards"), &row)) {
    return;
  }
  const bool8_t tiled = string_equals(perf->data.pipeline, "tiled");
  backend_card(editor, ui, 0u, "Draw calls", "draw.world.calls_issued", "",
               "per frame");
  backend_card(editor, ui, 1u, "Draw commands", "draw.world.commands_issued",
               "", "inside the calls");
  backend_card(editor, ui, 2u, "Graph build", "cpu.graph_build", "ms",
               "CPU per frame");
  backend_card(editor, ui, 3u, "Submit", "cpu.render_submit", "ms",
               "CPU per frame");
  backend_card(editor, ui, 4u, "Frame wait", "cpu.backend_frame_wait", "ms",
               "for a free frame slot");
  backend_card(editor, ui, 5u, tiled ? "Drawable" : "Image acquire",
               "cpu.drawable_acquire", "ms", "waiting for the swapchain");
  backend_card(editor, ui, 6u, "Present latency", "gpu.present.latency", "ms",
               tiled ? "from the presented handler" : "Metal only");
  (void)vkr_ui_panel_end(ui);
}

/* ------------------------------------------------------------------------ */
/* Passes                                                                    */

/* The shown frame's passes as one strip in submission order, each as wide
   as its share of the GPU time and coloured by kind. */
vkr_internal void backend_strip(VkrEditorUi *editor, VkrUiSystem *ui,
                                VkrUiPlacement placement, uint64_t frame) {
  const VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrRecordingData *data = &perf->data;
  const VkrUiTheme *theme = vkr_ui_theme();
  uint32_t count = 0u;
  const VkrRecordingPassRow *rows =
      vkr_recording_data_pass_rows(data, frame, &count);
  const bool8_t gpu = data->pass_gpu_frames > 0u;
  float32_t total = 0.0f;
  for (uint32_t i = 0u; i < count; ++i) {
    total += gpu ? rows[i].gpu_ms : rows[i].cpu_ms;
  }
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig area = vkr_editor_perf_grid(placement, &fill, 1u, &fill, 1u);
  area.style.background_color = theme->field;
  area.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  area.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("backend.strip"), &area)) {
    return;
  }
  /* The strip's width from its last layout. */
  const VkrUiId id =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("strip.fills"));
  VkrUiRect rect = {0};
  const float32_t scale = Max(ui->content_scale, 0.001f);
  const float32_t width =
      vkr_ui_widget_rect(ui, id, &rect) ? rect.width / scale : 600.0f;
  const float32_t height = 26.0f;
  VkrUiRect fills[VKR_RECORDING_PASS_MAX];
  uint8_t colors[VKR_RECORDING_PASS_MAX];
  float32_t x = 2.0f;
  const float32_t room = Max(width - 4.0f - (float32_t)count, 1.0f);
  for (uint32_t i = 0u; i < count && total > 0.0f; ++i) {
    const float32_t ms = gpu ? rows[i].gpu_ms : rows[i].cpu_ms;
    const float32_t w = Max(room * ms / total, 1.0f);
    fills[i] = (VkrUiRect){x, 4.0f, w, height};
    colors[i] = backend_kind(data, frame, rows[i].pass);
    x += w + 1.0f;
  }
  const VkrUiPathConfig path = {
      .palette = BACKEND_KIND_COLOR,
      .palette_count = ArrayCount(BACKEND_KIND_COLOR),
      .width_pt = 1.0f,
      .fills = fills,
      .fill_colors = colors,
      .fill_count = total > 0.0f ? count : 0u,
  };
  VkrUiWidgetConfig strip = vkr_ui_widget_config_default();
  strip.placement = vkr_editor_perf_cell(0u, 0u);
  (void)vkr_ui_path(ui, string8_lit("strip.fills"), &path, &strip);
  /* Names over the passes wide enough to hold them. */
  for (uint32_t i = 0u; i < count && total > 0.0f; ++i) {
    const char *name =
        rows[i].pass < data->pass_count ? data->passes[rows[i].pass].name : "";
    const float32_t text_width = 6.2f * (float32_t)string_length(name) + 8.0f;
    if (fills[i].width < text_width) {
      continue;
    }
    VkrUiWidgetConfig label =
        vkr_editor_text_config(theme->font_caption, theme->text_on_accent);
    label.placement = vkr_editor_perf_cell(0u, 0u);
    label.placement.justify = label.placement.align = VKR_UI_ALIGN_START;
    label.placement.margin_pt =
        (VkrUiEdges){4.0f + 6.0f, 0.0f, 0.0f, fills[i].x + 5.0f};
    label.style.max_size_pt = (Vec2){fills[i].width - 8.0f, 0.0f};
    (void)vkr_ui_push_id_u64(ui, i);
    vkr_ui_label(
        ui, string8_lit("name"),
        string8_create_from_cstr((const uint8_t *)name, string_length(name)),
        &label);
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

enum {
  PASS_ORDER = 0,
  PASS_NAME,
  PASS_KIND,
  PASS_GPU,
  PASS_SHARE,
  PASS_CPU,
  PASS_COLUMNS,
};

static const VkrEditorTableColumn PASS_TABLE[PASS_COLUMNS] = {
    {.title = "#",
     .width_pt = 34.0f,
     .min_pt = 28.0f,
     .numeric = true_v,
     .tooltip = "Submission order"},
    {.title = "Pass", .width_pt = 0.0f, .min_pt = 120.0f},
    {.title = "Kind",
     .width_pt = 66.0f,
     .min_pt = 50.0f,
     .tooltip = "The encoder or command buffer kind it records into"},
    {.title = "GPU ms", .width_pt = 66.0f, .min_pt = 50.0f, .numeric = true_v},
    {.title = "Share", .width_pt = 86.0f, .min_pt = 56.0f, .numeric = true_v},
    {.title = "Record CPU ms",
     .width_pt = 96.0f,
     .min_pt = 70.0f,
     .numeric = true_v,
     .tooltip = "CPU time recording the pass's commands"},
};

vkr_internal void backend_passes(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 VkrUiPlacement placement, uint64_t at) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrRecordingData *data = &perf->data;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrEditorTable *table = &perf->view.backend_passes;
  if (!table->initialized) {
    table->sort_column = VKR_EDITOR_TABLE_NONE;
  }
  uint32_t count = 0u;
  const VkrRecordingPassRow *rows =
      vkr_recording_data_pass_rows(data, at, &count);
  if (count == 0u) {
    vkr_editor_perf_empty(
        editor, ui, placement, VKR_UI_ICON_GPU, "No passes at this frame",
        data->pass_frames > 0u
            ? "This frame rendered no Scene: pick another frame."
            : "Recordings with Tier 1 metrics (the default) time every pass "
              "the backend records.");
    return;
  }
  const bool8_t gpu = data->pass_gpu_frames > 0u;
  float32_t total = 0.0f;
  for (uint32_t i = 0u; i < count; ++i) {
    total += gpu ? rows[i].gpu_ms : rows[i].cpu_ms;
  }
  total = Max(total, 1.0e-3f);
  float32_t largest = 1.0e-3f;
  for (uint32_t i = 0u; i < count; ++i) {
    largest = Max(largest, gpu ? rows[i].gpu_ms : rows[i].cpu_ms);
  }
  /* Submission order, or a clicked column. */
  uint32_t order[VKR_RECORDING_PASS_MAX];
  float64_t keys[VKR_RECORDING_PASS_MAX];
  for (uint32_t i = 0u; i < count; ++i) {
    order[i] = i;
    switch (table->sort_column) {
    case PASS_GPU:
    case PASS_SHARE:
      keys[i] = rows[i].gpu_ms;
      break;
    case PASS_CPU:
      keys[i] = rows[i].cpu_ms;
      break;
    case PASS_KIND:
      keys[i] = backend_kind(data, at, rows[i].pass) * 1000.0 + i;
      break;
    default:
      keys[i] = i;
      break;
    }
  }
  for (uint32_t i = 1u; i < count; ++i) {
    const uint32_t moved = order[i];
    uint32_t j = i;
    const bool8_t descending = table->sort_column != VKR_EDITOR_TABLE_NONE &&
                               table->sort_column != PASS_ORDER &&
                               table->sort_descending;
    while (j > 0u && (descending ? keys[order[j - 1u]] < keys[moved]
                                 : keys[order[j - 1u]] > keys[moved])) {
      order[j] = order[j - 1u];
      --j;
    }
    order[j] = moved;
  }
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  VkrEditorTableColumn columns[PASS_COLUMNS];
  MemCopy(columns, PASS_TABLE, sizeof(columns));
  if (!gpu) {
    columns[PASS_GPU].title = "CPU ms";
  }
  if (!vkr_editor_table_begin(ui, "backend.passes", table, columns,
                              PASS_COLUMNS, count, placement, &sorted, &first,
                              &end)) {
    return;
  }
  for (uint32_t i = first; i < end; ++i) {
    const uint32_t r = order[i];
    const VkrRecordingPassRow *row = &rows[r];
    const char *name =
        row->pass < data->pass_count ? data->passes[row->pass].name : "";
    const uint8_t kind = backend_kind(data, at, row->pass);
    const float32_t ms = gpu ? row->gpu_ms : row->cpu_ms;
    String8 cells[PASS_COLUMNS] = {
        string8_create_formatted(ui->frame_allocator, "%u", r + 1u),
        string8_create_from_cstr((const uint8_t *)name, string_length(name)),
        string8_create_from_cstr((const uint8_t *)BACKEND_KIND_NAME[kind],
                                 string_length(BACKEND_KIND_NAME[kind])),
        string8_create_formatted(ui->frame_allocator, "%.3f", ms),
        string8_create_formatted(ui->frame_allocator, "%.1f%%",
                                 100.0f * ms / total),
        string8_create_formatted(ui->frame_allocator, "%.3f", row->cpu_ms),
    };
    Vec4 colors[PASS_COLUMNS] = {0};
    colors[PASS_ORDER] = theme->text_disabled;
    colors[PASS_KIND] = theme->text_secondary;
    float32_t bars[PASS_COLUMNS];
    Vec4 bar_colors[PASS_COLUMNS] = {0};
    for (uint32_t c = 0u; c < PASS_COLUMNS; ++c) {
      bars[c] = -1.0f;
    }
    /* The bar measures against the slowest pass, so small ones show. */
    bars[PASS_SHARE] = ms / largest;
    bar_colors[PASS_SHARE] = BACKEND_KIND_COLOR[kind];
    const VkrEditorTableRow table_row = {
        .key = row->pass,
        .cells = cells,
        .cell_colors = colors,
        .cell_bars = bars,
        .bar_colors = bar_colors,
    };
    (void)vkr_editor_table_row(ui, table, columns, PASS_COLUMNS, i, &table_row);
  }
  vkr_editor_table_end(ui);
}

/* ------------------------------------------------------------------------ */
/* Recording cost, counters and functions                                    */

vkr_internal void backend_stages(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrEditorPerfBackend *backend = &perf->backend;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.value = VKR_EDITOR_PERF_CHART_TITLE_PT, .unit = VKR_UI_TRACK_PX}, fill};
  VkrUiPanelConfig panel =
      vkr_editor_perf_grid(placement, &fill, 1u, rows, ArrayCount(rows));
  panel.style.gap_pt = vkr_ui_theme()->space_xs;
  if (!vkr_ui_panel_begin(ui, string8_lit("backend.stages"), &panel)) {
    return;
  }
  vkr_editor_perf_chart_title(
      ui, "title", vkr_editor_perf_cell(0u, 0u),
      string8_create_formatted(ui->frame_allocator,
                               "Frame building and submission, CPU ms "
                               "stacked  \xc2\xb7  top %.2f ms",
                               backend->stage_top),
      BACKEND_STAGE_LABEL, BACKEND_STAGE_COLOR, VKR_EDITOR_PERF_RECORD_STAGES);
  VkrUiPlotSeries bars[VKR_EDITOR_PERF_RECORD_STAGES];
  for (uint32_t s = 0u; s < VKR_EDITOR_PERF_RECORD_STAGES; ++s) {
    bars[s] = (VkrUiPlotSeries){.values = backend->stage_ms[s],
                                .color = BACKEND_STAGE_COLOR[s]};
  }
  String8 tip = (String8){0};
  const uint32_t c = perf->hovered_column;
  if (c < perf->column_count) {
    tip = string8_create_formatted(ui->frame_allocator, "Frame %llu",
                                   (unsigned long long)perf->column_frame[c]);
    for (uint32_t s = 0u; s < VKR_EDITOR_PERF_RECORD_STAGES; ++s) {
      tip = string8_create_formatted(
          ui->frame_allocator, "%.*s\n%s  %.3f ms", (int32_t)tip.length,
          tip.str, BACKEND_STAGE_LABEL[s], backend->stage_ms[s][c]);
    }
  }
  VkrUiPlotConfig plot = {
      .sample_count = perf->column_count,
      .y_min = 0.0f,
      .y_max = backend->stage_top,
      .bars = bars,
      .bar_count = VKR_EDITOR_PERF_RECORD_STAGES,
  };
  vkr_editor_perf_plot(editor, frame, "stages.plot",
                       vkr_editor_perf_cell(0u, 1u), &plot, false_v, tip);
  (void)vkr_ui_panel_end(ui);
}

enum {
  COUNTER_NAME = 0,
  COUNTER_FRAME,
  COUNTER_MEAN,
  COUNTER_MAX,
  COUNTER_COLUMNS,
};

static const VkrEditorTableColumn COUNTER_TABLE[COUNTER_COLUMNS] = {
    {.title = "Counter", .width_pt = 0.0f, .min_pt = 140.0f},
    {.title = "This frame",
     .width_pt = 78.0f,
     .min_pt = 60.0f,
     .numeric = true_v},
    {.title = "Mean",
     .width_pt = 70.0f,
     .min_pt = 56.0f,
     .numeric = true_v,
     .tooltip = "Over the shown frames"},
    {.title = "Max",
     .width_pt = 70.0f,
     .min_pt = 56.0f,
     .numeric = true_v,
     .tooltip = "Over the shown frames"},
};

vkr_internal String8 backend_number(VkrUiSystem *ui, float64_t value,
                                    const char *unit) {
  if (string_equals(unit, "ms")) {
    return string8_create_formatted(ui->frame_allocator, "%.3f", value);
  }
  if (string_equals(unit, "bytes")) {
    return string8_create_formatted(ui->frame_allocator, "%.1f MB",
                                    value / (1024.0 * 1024.0));
  }
  return vkr_abs_f64(value - (float64_t)(int64_t)value) < 1.0e-6
             ? string8_create_formatted(ui->frame_allocator, "%.0f", value)
             : string8_create_formatted(ui->frame_allocator, "%.2f", value);
}

vkr_internal void backend_counters(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   VkrUiPlacement placement, uint64_t at) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrRecordingData *data = &perf->data;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  /* Backend counters first, then each prefix's in recorded order. */
  uint32_t series[BACKEND_COUNTERS_MAX];
  uint32_t count = 0u;
  for (uint32_t p = 0u; p < ArrayCount(BACKEND_COUNTER_PREFIXES); ++p) {
    const char *prefix = BACKEND_COUNTER_PREFIXES[p];
    const uint64_t length = string_length(prefix);
    for (uint32_t i = 0u;
         i < data->series_count && count < BACKEND_COUNTERS_MAX; ++i) {
      const char *name = data->series[i].name;
      if (string_length_bounded(name, length) == length &&
          MemCompare(name, prefix, length) == 0 &&
          perf->analysis.series[i].valid > 0u) {
        series[count++] = i;
      }
    }
  }
  if (count == 0u) {
    vkr_editor_perf_empty(editor, ui, placement, VKR_UI_ICON_GPU,
                          "No backend counters",
                          "Tier 1 recordings carry the backend's counters: "
                          "draws, pipelines, uploads and command slots.");
    return;
  }
  VkrEditorTable *table = &perf->view.backend_counters;
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, "backend.counters", table, COUNTER_TABLE,
                              COUNTER_COLUMNS, count, placement, &sorted,
                              &first, &end)) {
    return;
  }
  const uint64_t last = Min(perf->view_last, data->frame_count - 1u);
  for (uint32_t i = first; i < end; ++i) {
    const VkrRecordingSeries *s = &data->series[series[i]];
    float64_t sum = 0.0;
    float64_t max = 0.0;
    uint64_t valid = 0u;
    for (uint64_t f = perf->view_first; f <= last; ++f) {
      if (s->valid[f]) {
        sum += s->values[f];
        max = Max(max, s->values[f]);
        ++valid;
      }
    }
    String8 cells[COUNTER_COLUMNS] = {
        string8_create_from_cstr((const uint8_t *)s->name,
                                 string_length(s->name)),
        at < data->frame_count && s->valid[at]
            ? backend_number(ui, s->values[at], s->unit)
            : string8_lit("\xe2\x80\x94"),
        valid ? backend_number(ui, sum / (float64_t)valid, s->unit)
              : (String8){0},
        valid ? backend_number(ui, max, s->unit) : (String8){0},
    };
    Vec4 colors[COUNTER_COLUMNS] = {0};
    colors[COUNTER_NAME] = theme->text_secondary;
    const VkrEditorTableRow row = {
        .key = series[i],
        .cells = cells,
        .cell_colors = colors,
        .tooltip = s->unit[0]
                       ? string8_create_formatted(ui->frame_allocator,
                                                  "%s, in %s", s->name, s->unit)
                       : (String8){0},
    };
    (void)vkr_editor_table_row(ui, table, COUNTER_TABLE, COUNTER_COLUMNS, i,
                               &row);
  }
  vkr_editor_table_end(ui);
}

enum {
  FUNCTION_NAME = 0,
  FUNCTION_THREAD,
  FUNCTION_CALLS,
  FUNCTION_MS,
  FUNCTION_MAX,
  FUNCTION_COLUMNS,
};

static const VkrEditorTableColumn FUNCTION_TABLE[FUNCTION_COLUMNS] = {
    {.title = "Function",
     .width_pt = 0.0f,
     .min_pt = 130.0f,
     .tooltip = "A CPU zone: a function or block the engine times"},
    {.title = "Thread", .width_pt = 62.0f, .min_pt = 50.0f},
    {.title = "Calls/frame",
     .width_pt = 80.0f,
     .min_pt = 60.0f,
     .numeric = true_v},
    {.title = "ms/frame",
     .width_pt = 70.0f,
     .min_pt = 56.0f,
     .numeric = true_v,
     .tooltip = "Time in the zone per frame, its inner zones included"},
    {.title = "Max ms",
     .width_pt = 66.0f,
     .min_pt = 52.0f,
     .numeric = true_v,
     .tooltip = "The longest single call over the shown frames"},
};

typedef struct BackendFunction {
  uint32_t key;
  uint16_t zone;
  uint8_t thread;
  float64_t calls;
  float64_t ms;
  float32_t max_ms;
} BackendFunction;

vkr_internal int32_t backend_compare_functions(const void *lhs,
                                               const void *rhs) {
  const BackendFunction *a = lhs;
  const BackendFunction *b = rhs;
  if (a->ms != b->ms) {
    return a->ms > b->ms ? -1 : 1;
  }
  return a->key < b->key ? -1 : (a->key > b->key ? 1 : 0);
}

vkr_internal void backend_functions(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame,
                                    VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrRecordingData *data = &perf->data;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  if (!data->zone_block) {
    vkr_editor_perf_empty(
        editor, ui, placement, VKR_UI_ICON_TIMER, "No CPU zones recorded",
        "Record with zones (Record popup, or rec.start zones) to see every "
        "timed function, its calls per frame and its time.");
    return;
  }
  BackendFunction *functions = vkr_allocator_alloc(
      ui->frame_allocator, sizeof(BackendFunction) * BACKEND_FUNCTIONS_MAX,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!functions) {
    return;
  }
  /* Sum the shown frames' rows by zone and thread. */
  uint32_t count = 0u;
  uint64_t frames = 0u;
  const uint64_t last = Min(perf->view_last, data->frame_count - 1u);
  for (uint64_t f = perf->view_first; f <= last; ++f) {
    uint32_t row_count = 0u;
    const VkrRecordingZoneRow *rows =
        vkr_recording_data_zone_rows(data, f, &row_count);
    frames += row_count > 0u ? 1u : 0u;
    for (uint32_t r = 0u; r < row_count; ++r) {
      const uint32_t key = ((uint32_t)rows[r].zone << 2u) | rows[r].thread;
      uint32_t at = 0u;
      while (at < count && functions[at].key != key) {
        ++at;
      }
      if (at == count) {
        if (count == BACKEND_FUNCTIONS_MAX) {
          continue;
        }
        functions[count++] = (BackendFunction){
            .key = key, .zone = rows[r].zone, .thread = rows[r].thread};
      }
      functions[at].calls += rows[r].calls;
      functions[at].ms += rows[r].total_ms;
      functions[at].max_ms = Max(functions[at].max_ms, rows[r].max_ms);
    }
  }
  vkr_sort(functions, count, sizeof(BackendFunction),
           backend_compare_functions);
  VkrEditorTable *table = &perf->view.backend_functions;
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, "backend.functions", table, FUNCTION_TABLE,
                              FUNCTION_COLUMNS, count, placement, &sorted,
                              &first, &end)) {
    return;
  }
  static const char *const threads[] = {"main", "render", "worker"};
  const float64_t per = frames > 0u ? 1.0 / (float64_t)frames : 0.0;
  for (uint32_t i = first; i < end; ++i) {
    const BackendFunction *function = &functions[i];
    const char *name = function->zone < data->zone_name_count
                           ? data->zone_names[function->zone].name
                           : "";
    String8 cells[FUNCTION_COLUMNS] = {
        string8_create_from_cstr((const uint8_t *)name, string_length(name)),
        string8_create_from_cstr(
            (const uint8_t *)threads[Min(function->thread, (uint8_t)2u)],
            string_length(threads[Min(function->thread, (uint8_t)2u)])),
        string8_create_formatted(ui->frame_allocator, "%.1f",
                                 function->calls * per),
        string8_create_formatted(ui->frame_allocator, "%.3f",
                                 function->ms * per),
        string8_create_formatted(ui->frame_allocator, "%.3f", function->max_ms),
    };
    Vec4 colors[FUNCTION_COLUMNS] = {0};
    colors[FUNCTION_THREAD] = theme->text_secondary;
    const VkrEditorTableRow row = {
        .key = function->key,
        .cells = cells,
        .cell_colors = colors,
    };
    (void)vkr_editor_table_row(ui, table, FUNCTION_TABLE, FUNCTION_COLUMNS, i,
                               &row);
  }
  vkr_editor_table_end(ui);
}

/* A section title with an action on its right. */
vkr_internal bool8_t backend_title(VkrEditorUi *editor, VkrUiSystem *ui,
                                   const char *id, VkrUiPlacement placement,
                                   String8 title, const char *action,
                                   const char *tip) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack columns[] = {fill, auto_track};
  VkrUiPanelConfig row =
      vkr_editor_perf_grid(placement, columns, action ? 2u : 1u, &fill, 1u);
  bool8_t clicked = false_v;
  if (!vkr_ui_panel_begin(
          ui, string8_create_from_cstr((const uint8_t *)id, string_length(id)),
          &row)) {
    return false_v;
  }
  vkr_editor_perf_caption(ui, "title", title, vkr_editor_perf_cell(0u, 0u),
                          theme->text_secondary);
  if (action) {
    VkrUiWidgetConfig button = vkr_ui_widget_config_default();
    button.placement = vkr_editor_perf_cell(1u, 0u);
    button.placement.align = VKR_UI_ALIGN_CENTER;
    vkr_editor_ghost_style(&button);
    button.style.font_size_pt = theme->font_caption;
    button.style.padding_pt = (VkrUiEdges){2.0f, 8.0f, 2.0f, 8.0f};
    button.tooltip =
        string8_create_from_cstr((const uint8_t *)tip, string_length(tip));
    clicked = vkr_ui_button(ui, string8_lit("action"),
                            string8_create_from_cstr((const uint8_t *)action,
                                                     string_length(action)),
                            &button);
  }
  (void)vkr_ui_panel_end(ui);
  (void)editor;
  return clicked;
}

/* ------------------------------------------------------------------------ */
/* Tab                                                                       */

void vkr_editor_recording_perf_backend_build(VkrEditorUi *editor,
                                             const VkrSampleUiFrame *frame,
                                             VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrRecordingData *data = &perf->data;
  backend_update(perf);
  const uint64_t focus = vkr_editor_perf_focus(perf, frame);
  const uint64_t at = backend_pass_frame(
      data, focus < data->frame_count ? focus : perf->view_last);
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack title = {.value = 20.0f, .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack rows[] = {{.value = 62.0f, .unit = VKR_UI_TRACK_PX}, fill};
  VkrUiPanelConfig body =
      vkr_editor_perf_grid(placement, &fill, 1u, rows, ArrayCount(rows));
  body.style.gap_pt = theme->space_sm;
  if (!vkr_ui_panel_begin(ui, string8_lit("backend.body"), &body)) {
    return;
  }
  backend_cards(editor, ui, vkr_editor_perf_cell(0u, 0u));
  const VkrUiTrack columns[] = {{.value = 1.1f, .unit = VKR_UI_TRACK_FR}, fill};
  VkrUiPanelConfig split = vkr_editor_perf_grid(
      vkr_editor_perf_cell(0u, 1u), columns, ArrayCount(columns), &fill, 1u);
  split.style.gap_pt = theme->space_md;
  if (vkr_ui_panel_begin(ui, string8_lit("backend.split"), &split)) {
    /* Left: the shown frame's passes. */
    const VkrUiTrack left_rows[] = {
        title, {.value = 34.0f, .unit = VKR_UI_TRACK_PX}, fill};
    VkrUiPanelConfig left =
        vkr_editor_perf_grid(vkr_editor_perf_cell(0u, 0u), &fill, 1u, left_rows,
                             ArrayCount(left_rows));
    left.style.gap_pt = theme->space_xs;
    if (vkr_ui_panel_begin(ui, string8_lit("backend.left"), &left)) {
      uint32_t count = 0u;
      (void)vkr_recording_data_pass_rows(data, at, &count);
      (void)backend_title(
          editor, ui, "backend.passes.title", vkr_editor_perf_cell(0u, 0u),
          string8_create_formatted(
              ui->frame_allocator,
              "%s passes of frame %llu in submission order  \xc2\xb7  %u "
              "passes%s",
              vkr_editor_perf_backend_name(perf), (unsigned long long)at, count,
              focus < data->frame_count && at != focus
                  ? "  \xc2\xb7  the nearest frame with GPU times"
                  : ""),
          NULL, NULL);
      backend_strip(editor, ui, vkr_editor_perf_cell(0u, 1u), at);
      backend_passes(editor, frame, vkr_editor_perf_cell(0u, 2u), at);
      (void)vkr_ui_panel_end(ui);
    }
    /* Right: recording cost, counters and functions. */
    const VkrUiTrack right_rows[] = {
        {.value = 0.8f, .unit = VKR_UI_TRACK_FR}, title,
        {.value = 1.0f, .unit = VKR_UI_TRACK_FR}, title,
        {.value = 1.2f, .unit = VKR_UI_TRACK_FR},
    };
    VkrUiPanelConfig right =
        vkr_editor_perf_grid(vkr_editor_perf_cell(1u, 0u), &fill, 1u,
                             right_rows, ArrayCount(right_rows));
    right.style.gap_pt = theme->space_xs;
    if (vkr_ui_panel_begin(ui, string8_lit("backend.right"), &right)) {
      if (perf->backend.has_stages) {
        backend_stages(editor, frame, vkr_editor_perf_cell(0u, 0u));
      } else {
        vkr_editor_perf_empty(editor, ui, vkr_editor_perf_cell(0u, 0u),
                              VKR_UI_ICON_CPU, "No CPU stages recorded",
                              "Tier 0 recordings carry them.");
      }
      (void)backend_title(
          editor, ui, "backend.counters.title", vkr_editor_perf_cell(0u, 1u),
          string8_create_formatted(
              ui->frame_allocator, "%s counters  \xc2\xb7  frame %llu",
              vkr_editor_perf_backend_name(perf), (unsigned long long)at),
          NULL, NULL);
      backend_counters(editor, frame, vkr_editor_perf_cell(0u, 2u), at);
      if (backend_title(editor, ui, "backend.functions.title",
                        vkr_editor_perf_cell(0u, 3u),
                        string8_lit("Function invocations over the shown "
                                    "frames, from CPU zones"),
                        data->zone_block ? "Open Thread timeline" : NULL,
                        "Show these zones on each thread's timeline")) {
        vkr_editor_window_set_visible(editor, VKR_EDITOR_WINDOW_TIMELINE,
                                      true_v);
        vkr_editor_perf_select(editor, frame, at);
      }
      backend_functions(editor, frame, vkr_editor_perf_cell(0u, 4u));
      (void)vkr_ui_panel_end(ui);
    }
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_panel_end(ui);
}
