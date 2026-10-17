#include "editor_recording.h"

#include "core/vkr_content_codec.h"
#include "editor_internal.h"
#include "editor_ui.h"
#include "memory/vkr_dmemory_allocator.h"

/* The Performance window (docs/proposals/recordings.md, "Performance
   view"): a recording's statistics, its frame-time, CPU-stage and memory
   charts and its spikes. A click on a chart or a spike seeks the playback to
   that frame, opening the recording first. */

#define PERF_MEMORY_COMMIT (1ull * 1024ull * 1024ull)
#define PERF_MEMORY_RESERVE (1024ull * 1024ull * 1024ull)
#define PERF_GB (1024.0 * 1024.0 * 1024.0)

/* Frame-time bar colors: within budget, within twice, over, no sample. */
enum {
  PERF_FRAME_OK = 0u,
  PERF_FRAME_SLOW,
  PERF_FRAME_HITCH,
  PERF_FRAME_NONE,
};

static const char *const PERF_STAGE_SERIES[VKR_EDITOR_PERF_STAGES] = {
    "cpu.update",
    "cpu.render_prepare",
    "cpu.render_submit",
    "cpu.render_wait",
};

static const char *const PERF_STAGE_LABEL[VKR_EDITOR_PERF_STAGES] = {
    "Update",
    "Render prepare",
    "Submit",
    "Render wait",
};

/* Categorical series colors: the first four slots of the dataviz reference
   palette's dark steps, in its fixed order. */
static const Vec4 PERF_STAGE_COLOR[VKR_EDITOR_PERF_STAGES] = {
    {0.224f, 0.529f, 0.898f, 1.0f},
    {0.851f, 0.349f, 0.149f, 1.0f},
    {0.098f, 0.620f, 0.439f, 1.0f},
    {0.788f, 0.522f, 0.000f, 1.0f},
};

/* ------------------------------------------------------------------------ */
/* Data                                                                      */

void vkr_editor_recording_perf_release(VkrEditorRecordingPerf *perf) {
  if (perf->loaded) {
    vkr_recording_analysis_free(&perf->analysis);
    vkr_recording_data_free(&perf->data);
    vkr_dmemory_allocator_destroy(&perf->allocator);
  }
  perf->loaded = false_v;
  perf->column_count = 0u;
}

/* The value of series `index` at `frame`, or false_v without a sample. */
vkr_internal bool8_t perf_value(const VkrRecordingData *data, uint32_t index,
                                uint64_t frame, float64_t *out) {
  if (index == UINT32_MAX || frame >= data->frame_count ||
      !data->series[index].valid[frame]) {
    return false_v;
  }
  *out = data->series[index].values[frame];
  return true_v;
}

vkr_internal void perf_range(float32_t *range, float32_t value) {
  range[0] = Min(range[0], value);
  range[1] = Max(range[1], value);
}

/* Reduces the recording to at most VKR_EDITOR_PERF_COLUMNS columns, each
   showing the slowest frame of its run so no spike disappears. */
vkr_internal void perf_columns(VkrEditorRecordingPerf *perf) {
  const VkrRecordingData *data = &perf->data;
  const uint64_t frames = data->frame_count;
  const uint32_t columns =
      (uint32_t)Min(frames, (uint64_t)VKR_EDITOR_PERF_COLUMNS);
  uint32_t stage[VKR_EDITOR_PERF_STAGES];
  for (uint32_t s = 0u; s < VKR_EDITOR_PERF_STAGES; ++s) {
    stage[s] = vkr_recording_data_find_series(data, PERF_STAGE_SERIES[s]);
    perf->has_stages = perf->has_stages || stage[s] != UINT32_MAX;
  }
  const uint32_t ram =
      vkr_recording_data_find_series(data, "process.resident_bytes");
  const uint32_t vram = vkr_recording_data_find_series(
      data, "memory.gpu.driver.current_allocated_bytes");
  const float64_t budget = perf->analysis.budget_ms;
  perf->ram_range[0] = perf->vram_range[0] = 1.0e30f;
  perf->ram_range[1] = perf->vram_range[1] = 0.0f;
  float32_t ram_last = 0.0f;
  float32_t vram_last = 0.0f;
  float32_t stage_top = 0.0f;
  for (uint32_t c = 0u; c < columns; ++c) {
    const uint64_t first = (uint64_t)c * frames / columns;
    const uint64_t last =
        Max(first + 1u, (uint64_t)(c + 1u) * frames / columns);
    uint64_t worst = first;
    float64_t worst_ms = -1.0;
    for (uint64_t f = first; f < last; ++f) {
      float64_t ms = 0.0;
      if (vkr_recording_data_frame_ms(data, f, &ms) && ms > worst_ms) {
        worst_ms = ms;
        worst = f;
      }
    }
    perf->column_frame[c] = worst;
    perf->frame_ms[c] = worst_ms >= 0.0 ? (float32_t)worst_ms : 0.0f;
    perf->frame_color[c] = worst_ms < 0.0            ? PERF_FRAME_NONE
                           : worst_ms > 2.0 * budget ? PERF_FRAME_HITCH
                           : worst_ms > budget       ? PERF_FRAME_SLOW
                                                     : PERF_FRAME_OK;
    float32_t stage_sum = 0.0f;
    for (uint32_t s = 0u; s < VKR_EDITOR_PERF_STAGES; ++s) {
      float64_t value = 0.0;
      perf->stage_ms[s][c] =
          perf_value(data, stage[s], worst, &value) ? (float32_t)value : 0.0f;
      stage_sum += perf->stage_ms[s][c];
    }
    stage_top = Max(stage_top, stage_sum);
    float64_t bytes = 0.0;
    if (perf_value(data, ram, worst, &bytes)) {
      ram_last = (float32_t)(bytes / PERF_GB);
      perf->has_ram = true_v;
      perf_range(perf->ram_range, ram_last);
    }
    if (perf_value(data, vram, worst, &bytes)) {
      vram_last = (float32_t)(bytes / PERF_GB);
      perf->has_vram = true_v;
      perf_range(perf->vram_range, vram_last);
    }
    perf->ram_gb[c] = ram_last;
    perf->vram_gb[c] = vram_last;
  }
  perf->column_count = columns;
  /* One giant hitch must not flatten every other bar: the range stops at
     three times the 99th percentile; taller bars clip at the top. */
  const VkrRecordingStats *time = &perf->analysis.frame_time;
  perf->frame_ms_top = (float32_t)Max(
      budget * 1.5, Min(time->max * 1.05, Max(time->p99 * 3.0, budget * 2.2)));
  perf->stage_ms_top = Max(stage_top * 1.05f, 1.0f);
}

bool8_t vkr_editor_recording_analyze(VkrEditorUi *editor, const char *name,
                                     char *error, uint64_t error_capacity) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  char path[VKR_RECORDING_PATH_MAX];
  if (!vkr_editor_recording_resolve(editor, name, path, sizeof(path))) {
    string_format(error, error_capacity, "No recording named '%s'.",
                  name ? name : "");
    return false_v;
  }
  vkr_editor_recording_perf_release(perf);
  MemZero(perf, sizeof(*perf));
  perf->selected = UINT64_MAX;
  if (!vkr_dmemory_create(PERF_MEMORY_COMMIT, PERF_MEMORY_RESERVE,
                          &perf->memory)) {
    string_format(error, error_capacity, "Out of memory.");
    return false_v;
  }
  perf->allocator = (VkrAllocator){.ctx = &perf->memory};
  vkr_dmemory_allocator_create(&perf->allocator);
  if (vkr_recording_data_load(&perf->data, &perf->allocator, path,
                              vkr_content_codec_zstd_decoder()) !=
          VKR_RECORDING_DATA_OK ||
      perf->data.frame_count == 0u) {
    string_format(error, error_capacity, "'%s' is not a readable recording.",
                  path);
    vkr_dmemory_allocator_destroy(&perf->allocator);
    return false_v;
  }
  if (!vkr_recording_analyze(&perf->data, NULL, &perf->allocator,
                             &perf->analysis)) {
    string_format(error, error_capacity,
                  "The recording could not be analyzed.");
    vkr_recording_data_free(&perf->data);
    vkr_dmemory_allocator_destroy(&perf->allocator);
    return false_v;
  }
  perf->loaded = true_v;
  string_format(perf->path, sizeof(perf->path), "%s", path);
  /* The bundle's name, without its directory and extension. */
  const char *base = path;
  for (const char *c = path; *c; ++c) {
    if (*c == '/' || *c == '\\') {
      base = c + 1;
    }
  }
  string_format(perf->name, sizeof(perf->name), "%s", base);
  const uint64_t length = string_length(perf->name);
  const uint64_t extension = sizeof(VKR_RECORDING_BUNDLE_EXTENSION) - 1u;
  if (length > extension && string_equals(perf->name + length - extension,
                                          VKR_RECORDING_BUNDLE_EXTENSION)) {
    perf->name[length - extension] = '\0';
  }
  perf_columns(perf);
  vkr_editor_window_set_visible(editor, VKR_EDITOR_WINDOW_PERFORMANCE, true_v);
  return true_v;
}

/* ------------------------------------------------------------------------ */
/* Seeking                                                                   */

vkr_internal const VkrRecordingPlayback *
perf_playback(const VkrEditorRecordingPerf *perf,
              const VkrSampleUiFrame *frame) {
  const VkrRecordingPlayback *playback =
      frame->recording ? frame->recording->playback : NULL;
  return playback && string_equals(playback->bundle_path, perf->path) ? playback
                                                                      : NULL;
}

/* Shows `target` in the Scene: seeks the open playback of this recording,
   or opens it and then seeks. */
vkr_internal void perf_seek(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                            uint64_t target) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  perf->selected = target;
  if (!perf_playback(perf, frame)) {
    if (frame->recording && frame->recording->recording) {
      vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL,
                       vkr_ui_theme()->warning,
                       "Stop the recording to watch this frame.");
      return;
    }
    VkrSampleRecordingRequest open = {.command = VKR_SAMPLE_RECORDING_OPEN};
    string_format(open.path, sizeof(open.path), "%s", perf->path);
    (void)vkr_editor_recording_send(editor, open, true_v);
  }
  (void)vkr_editor_recording_send(
      editor,
      (VkrSampleRecordingRequest){.command = VKR_SAMPLE_RECORDING_SEEK_FRAME,
                                  .frame = target},
      false_v);
}

/* The column showing `frame`, or VKR_UI_PLOT_NONE. */
vkr_internal uint32_t perf_column_of(const VkrEditorRecordingPerf *perf,
                                     uint64_t frame) {
  if (frame == UINT64_MAX || perf->column_count == 0u) {
    return VKR_UI_PLOT_NONE;
  }
  const uint64_t frames = perf->data.frame_count;
  return (uint32_t)Min(frame * perf->column_count / Max(frames, 1u),
                       (uint64_t)perf->column_count - 1u);
}

/* ------------------------------------------------------------------------ */
/* Widgets                                                                   */

vkr_internal VkrUiPlacement perf_cell(uint32_t column, uint32_t row) {
  return (VkrUiPlacement){.column = column,
                          .row = row,
                          .column_span = 1u,
                          .row_span = 1u,
                          .justify = VKR_UI_ALIGN_STRETCH,
                          .align = VKR_UI_ALIGN_STRETCH};
}

vkr_internal VkrUiPanelConfig perf_grid(VkrUiPlacement placement,
                                        const VkrUiTrack *columns,
                                        uint32_t column_count,
                                        const VkrUiTrack *rows,
                                        uint32_t row_count) {
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = placement;
  panel.columns = columns;
  panel.column_count = column_count;
  panel.rows = rows;
  panel.row_count = row_count;
  panel.style.padding_pt = (VkrUiEdges){0};
  return panel;
}

vkr_internal void perf_caption(VkrUiSystem *ui, const char *id, String8 text,
                               VkrUiPlacement placement, Vec4 color) {
  VkrUiWidgetConfig label =
      vkr_editor_text_config(vkr_ui_theme()->font_caption, color);
  label.placement = placement;
  label.placement.justify = VKR_UI_ALIGN_START;
  label.placement.align = VKR_UI_ALIGN_CENTER;
  vkr_ui_label(ui,
               string8_create_from_cstr((const uint8_t *)id, string_length(id)),
               text, &label);
}

/* A stat card: a caption over a value. */
vkr_internal void perf_card(VkrEditorUi *editor, VkrUiSystem *ui,
                            uint32_t column, const char *caption, String8 value,
                            Vec4 color) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {{.value = 14.0f, .unit = VKR_UI_TRACK_PX},
                             {.value = 1.0f, .unit = VKR_UI_TRACK_FR}};
  VkrUiPanelConfig card =
      perf_grid(perf_cell(column, 0u), &fill, 1u, rows, ArrayCount(rows));
  card.style.padding_pt = (VkrUiEdges){6.0f, 10.0f, 6.0f, 10.0f};
  card.style.background_color = theme->raised;
  card.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  card.style.border_color = theme->border;
  card.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  (void)vkr_ui_push_id_u64(ui, column);
  if (vkr_ui_panel_begin(ui, string8_lit("card"), &card)) {
    perf_caption(ui, "caption",
                 string8_create_from_cstr((const uint8_t *)caption,
                                          string_length(caption)),
                 perf_cell(0u, 0u), theme->text_secondary);
    VkrUiWidgetConfig text =
        vkr_editor_text_config(theme->font_emphasis, color);
    text.placement = perf_cell(0u, 1u);
    text.placement.justify = VKR_UI_ALIGN_START;
    text.placement.align = VKR_UI_ALIGN_CENTER;
    text.text.font = editor->heading_font;
    vkr_ui_label(ui, string8_lit("value"), value, &text);
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_pop_id(ui);
}

vkr_internal String8 perf_gb(VkrUiSystem *ui, const VkrRecordingData *data,
                             const VkrRecordingAnalysis *analysis,
                             const char *series) {
  const uint32_t index = vkr_recording_data_find_series(data, series);
  if (index == UINT32_MAX || analysis->series[index].valid == 0u) {
    return string8_lit("n/a");
  }
  const VkrRecordingStats *stats = &analysis->series[index];
  return string8_create_formatted(ui->frame_allocator, "%.2f / %.2f GB",
                                  stats->max / PERF_GB, stats->mean / PERF_GB);
}

vkr_internal void perf_cards(VkrEditorUi *editor, VkrUiSystem *ui,
                             uint32_t row) {
  const VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrRecordingAnalysis *analysis = &perf->analysis;
  const VkrRecordingStats *time = &analysis->frame_time;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack card = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack wide = {.value = 1.6f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack columns[] = {card, card, card, card, card, card, wide, wide};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig cards =
      perf_grid(perf_cell(0u, row), columns, ArrayCount(columns), &fill, 1u);
  cards.style.gap_pt = theme->space_sm;
  if (!vkr_ui_panel_begin(ui, string8_lit("cards"), &cards)) {
    return;
  }
  const float64_t budget = analysis->budget_ms;
  const Vec4 p99_color = time->p99 > budget ? theme->warning : theme->text;
  const Vec4 max_color = time->max > 2.0 * budget ? theme->error
                         : time->max > budget     ? theme->warning
                                                  : theme->text;
  perf_card(editor, ui, 0u, "p50",
            string8_create_formatted(ui->frame_allocator, "%.2f ms", time->p50),
            theme->text);
  perf_card(editor, ui, 1u, "p95",
            string8_create_formatted(ui->frame_allocator, "%.2f ms", time->p95),
            theme->text);
  perf_card(editor, ui, 2u, "p99",
            string8_create_formatted(ui->frame_allocator, "%.2f ms", time->p99),
            p99_color);
  perf_card(editor, ui, 3u, "max",
            string8_create_formatted(ui->frame_allocator, "%.2f ms", time->max),
            max_color);
  perf_card(editor, ui, 4u, "1% low",
            string8_create_formatted(ui->frame_allocator, "%.2f ms",
                                     analysis->low_1pct_ms),
            theme->text);
  perf_card(editor, ui, 5u, "over 16.7 ms",
            string8_create_formatted(ui->frame_allocator, "%llu frames",
                                     (unsigned long long)analysis->over_budget),
            analysis->over_budget ? theme->warning : theme->text);
  perf_card(editor, ui, 6u, "RAM peak / avg",
            perf_gb(ui, &perf->data, analysis, "process.resident_bytes"),
            theme->text);
  perf_card(editor, ui, 7u, "VRAM peak / avg",
            perf_gb(ui, &perf->data, analysis,
                    "memory.gpu.driver.current_allocated_bytes"),
            theme->text);
  (void)vkr_ui_panel_end(ui);
}

/* The tooltip of a hovered column: the frame, its time and its stages. */
vkr_internal String8 perf_tooltip(VkrUiSystem *ui,
                                  const VkrEditorRecordingPerf *perf,
                                  uint32_t column) {
  if (column >= perf->column_count) {
    return (String8){0};
  }
  const uint64_t f = perf->column_frame[column];
  String8 text = string8_create_formatted(
      ui->frame_allocator, "Frame %llu  \xc2\xb7  %.3f s  \xc2\xb7  %.2f ms",
      (unsigned long long)f, perf->data.times[f], perf->frame_ms[column]);
  if (perf->has_stages) {
    text = string8_create_formatted(
        ui->frame_allocator,
        "%.*s\nUpdate %.2f  \xc2\xb7  Prepare %.2f  \xc2\xb7  Submit %.2f  "
        "\xc2\xb7  Wait %.2f ms",
        (int32_t)text.length, text.str, perf->stage_ms[0][column],
        perf->stage_ms[1][column], perf->stage_ms[2][column],
        perf->stage_ms[3][column]);
  }
  if (perf->has_ram || perf->has_vram) {
    text = string8_create_formatted(
        ui->frame_allocator, "%.*s\nRAM %.2f GB  \xc2\xb7  VRAM %.2f GB",
        (int32_t)text.length, text.str, perf->ram_gb[column],
        perf->vram_gb[column]);
  }
  return string8_create_formatted(ui->frame_allocator, "%.*s\nClick to watch",
                                  (int32_t)text.length, text.str);
}

/* A plot that seeks on click and describes the hovered column. */
vkr_internal void perf_plot(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                            const char *id, VkrUiPlacement placement,
                            VkrUiPlotConfig *plot) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrRecordingPlayback *playback = perf_playback(perf, frame);
  plot->cursor = perf_column_of(
      perf, playback ? vkr_recording_playback_frame_at(playback, playback->time)
                     : perf->selected);
  plot->cursor_color = theme->accent;
  plot->hover_color = vkr_ui_color_alpha(theme->text, 0.08f);
  plot->range_first = plot->range_last = VKR_UI_PLOT_NONE;
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement = placement;
  config.style.background_color = theme->field;
  config.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  config.style.padding_pt = (VkrUiEdges){4.0f, 4.0f, 4.0f, 4.0f};
  /* The tooltip describes the column the pointer was over last build. */
  config.tooltip = perf_tooltip(ui, perf, perf->hovered_column);
  const VkrUiPlotResult result = vkr_ui_plot(
      ui, string8_create_from_cstr((const uint8_t *)id, string_length(id)),
      plot, &config);
  if (result.hovered != VKR_UI_PLOT_NONE) {
    perf->hovered_column = result.hovered;
  }
  if (result.clicked != VKR_UI_PLOT_NONE &&
      result.clicked < perf->column_count) {
    perf_seek(editor, frame, perf->column_frame[result.clicked]);
  }
}

/* A chart's title row: its name and, for stacks, a legend. */
vkr_internal void perf_title(VkrUiSystem *ui, const char *id, String8 text,
                             VkrUiPlacement placement, bool8_t legend) {
  const VkrUiTheme *theme = vkr_ui_theme();
  if (!legend) {
    perf_caption(ui, id, text, placement, theme->text_secondary);
    return;
  }
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack columns[] = {fill, auto_track, auto_track, auto_track,
                                auto_track};
  VkrUiPanelConfig row =
      perf_grid(placement, columns, ArrayCount(columns), &fill, 1u);
  row.style.gap_pt = theme->space_md;
  if (!vkr_ui_panel_begin(
          ui, string8_create_from_cstr((const uint8_t *)id, string_length(id)),
          &row)) {
    return;
  }
  perf_caption(ui, "name", text, perf_cell(0u, 0u), theme->text_secondary);
  for (uint32_t s = 0u; s < VKR_EDITOR_PERF_STAGES; ++s) {
    VkrUiWidgetConfig key =
        vkr_editor_text_config(theme->font_caption, theme->text_secondary);
    key.placement = perf_cell(1u + s, 0u);
    key.placement.align = VKR_UI_ALIGN_CENTER;
    key.icon = VKR_UI_ICON_SQUARE;
    key.icon_size_pt = 10.0f;
    key.icon_color = PERF_STAGE_COLOR[s];
    (void)vkr_ui_push_id_u64(ui, s);
    vkr_ui_label(ui, string8_lit("key"),
                 string8_create_from_cstr((const uint8_t *)PERF_STAGE_LABEL[s],
                                          string_length(PERF_STAGE_LABEL[s])),
                 &key);
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

vkr_internal void perf_frame_chart(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   uint32_t row) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float64_t budget = perf->analysis.budget_ms;
  perf_title(frame->ui, "frame.title",
             string8_create_formatted(
                 frame->ui->frame_allocator,
                 "Frame time (ms, %s)  \xc2\xb7  dashed at %.1f ms and "
                 "twice it; green within, amber up to twice, red beyond",
                 vkr_recording_data_frame_source(&perf->data), budget),
             perf_cell(0u, row), false_v);
  const Vec4 palette[] = {theme->success, theme->warning, theme->error,
                          theme->text_disabled};
  const VkrUiPlotSeries bars = {.values = perf->frame_ms,
                                .color = theme->success};
  const VkrUiPlotReference references[] = {
      {.value = (float32_t)budget,
       .color = vkr_ui_color_alpha(theme->text_secondary, 0.7f),
       .dashed = true_v},
      {.value = (float32_t)(2.0 * budget),
       .color = vkr_ui_color_alpha(theme->text_secondary, 0.5f),
       .dashed = true_v},
  };
  VkrUiPlotConfig plot = {
      .sample_count = perf->column_count,
      .y_min = 0.0f,
      .y_max = perf->frame_ms_top,
      .bars = &bars,
      .bar_count = 1u,
      .bar_colors = perf->frame_color,
      .palette = palette,
      .palette_count = ArrayCount(palette),
      .references = references,
      .reference_count = ArrayCount(references),
  };
  perf_plot(editor, frame, "frame.plot", perf_cell(0u, row + 1u), &plot);
}

vkr_internal void perf_stage_chart(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   uint32_t row) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  perf_title(frame->ui, "stage.title",
             string8_lit("CPU by stage (ms), stacked"), perf_cell(0u, row),
             true_v);
  VkrUiPlotSeries bars[VKR_EDITOR_PERF_STAGES];
  for (uint32_t s = 0u; s < VKR_EDITOR_PERF_STAGES; ++s) {
    bars[s] = (VkrUiPlotSeries){.values = perf->stage_ms[s],
                                .color = PERF_STAGE_COLOR[s]};
  }
  VkrUiPlotConfig plot = {
      .sample_count = perf->column_count,
      .y_min = 0.0f,
      .y_max = perf->stage_ms_top,
      .bars = bars,
      .bar_count = VKR_EDITOR_PERF_STAGES,
  };
  perf_plot(editor, frame, "stage.plot", perf_cell(0u, row + 1u), &plot);
}

/* One memory chart: a line on its own axis, with its range in the title. */
vkr_internal void
perf_memory_chart(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                  uint32_t column, const char *name, const float32_t *values,
                  const float32_t *range, bool8_t present, Vec4 color) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {{.value = 18.0f, .unit = VKR_UI_TRACK_PX}, fill};
  VkrUiPanelConfig cell =
      perf_grid(perf_cell(column, 0u), &fill, 1u, rows, ArrayCount(rows));
  (void)vkr_ui_push_id_u64(ui, column);
  if (!vkr_ui_panel_begin(ui, string8_lit("memory"), &cell)) {
    (void)vkr_ui_pop_id(ui);
    return;
  }
  perf_title(ui, "title",
             present
                 ? string8_create_formatted(ui->frame_allocator,
                                            "%s (GB)  \xc2\xb7  %.2f to %.2f",
                                            name, range[0], range[1])
                 : string8_create_formatted(ui->frame_allocator,
                                            "%s: not recorded", name),
             perf_cell(0u, 0u), false_v);
  if (present) {
    /* A flat line sits mid-height instead of on an edge. */
    const float32_t pad = Max((range[1] - range[0]) * 0.1f, 0.01f);
    const VkrUiPlotSeries line = {.values = values, .color = color};
    VkrUiPlotConfig plot = {
        .sample_count = editor->recording.perf.column_count,
        .y_min = range[0] - pad,
        .y_max = range[1] + pad,
        .lines = &line,
        .line_count = 1u,
        .line_width_pt = 2.0f,
    };
    perf_plot(editor, frame, "plot", perf_cell(0u, 1u), &plot);
  }
  (void)vkr_ui_panel_end(ui);
  (void)vkr_ui_pop_id(ui);
}

vkr_internal void perf_memory_charts(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     uint32_t row) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack columns[] = {fill, fill};
  VkrUiPanelConfig pair =
      perf_grid(perf_cell(0u, row), columns, ArrayCount(columns), &fill, 1u);
  pair.style.gap_pt = vkr_ui_theme()->space_md;
  if (!vkr_ui_panel_begin(frame->ui, string8_lit("memory.row"), &pair)) {
    return;
  }
  perf_memory_chart(editor, frame, 0u, "RAM", perf->ram_gb, perf->ram_range,
                    perf->has_ram, PERF_STAGE_COLOR[0]);
  perf_memory_chart(editor, frame, 1u, "VRAM", perf->vram_gb, perf->vram_range,
                    perf->has_vram, PERF_STAGE_COLOR[1]);
  (void)vkr_ui_panel_end(frame->ui);
}

/* One spike row: rank, frame, time, length, ratio, main cause and marker. */
vkr_internal String8 perf_spike_text(VkrUiSystem *ui,
                                     const VkrEditorRecordingPerf *perf,
                                     uint32_t index) {
  const VkrRecordingSpike *spike = &perf->analysis.spikes[index];
  char cause[96] = "";
  if (spike->contributor_count > 0u) {
    const VkrRecordingContributor *top = &spike->contributors[0];
    string_format(cause, sizeof(cause), "%s +%.2f ms",
                  perf->data.series[top->series].name, top->excess);
  }
  const char *marker = "";
  for (uint32_t m = 0u; m < perf->data.marker_count; ++m) {
    const VkrRecordingDataMarker *near = &perf->data.markers[m];
    const uint64_t distance = near->frame > spike->frame
                                  ? near->frame - spike->frame
                                  : spike->frame - near->frame;
    if (distance <= 10u) {
      marker = near->label;
      break;
    }
  }
  return string8_create_formatted(
      ui->frame_allocator,
      "#%-3u frame %-7llu %8.2f s %8.2f ms  x%-5.1f %s%s%s", index + 1u,
      (unsigned long long)spike->frame, spike->time, spike->frame_ms,
      spike->ratio, cause, marker[0] ? "  \xe2\x9a\x91 " : "", marker);
}

vkr_internal void perf_spikes(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame, uint32_t row) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrRecordingAnalysis *analysis = &perf->analysis;
  perf_title(ui, "spikes.title",
             string8_create_formatted(
                 ui->frame_allocator,
                 "Spikes: %u of %llu  \xc2\xb7  over 1.2x the median of 19 "
                 "neighbours and 4 ms above it, or over twice the budget",
                 analysis->spike_count,
                 (unsigned long long)analysis->spikes_found),
             perf_cell(0u, row), false_v);
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiTrack rows[VKR_RECORDING_ANALYSIS_SPIKE_MAX];
  const uint32_t count =
      Min(analysis->spike_count, (uint32_t)VKR_RECORDING_ANALYSIS_SPIKE_MAX);
  for (uint32_t i = 0u; i < count; ++i) {
    rows[i] = (VkrUiTrack){.value = 22.0f, .unit = VKR_UI_TRACK_PX};
  }
  if (count == 0u) {
    perf_caption(ui, "spikes.none",
                 string8_lit("No spikes: every frame stays near its "
                             "neighbours."),
                 perf_cell(0u, row + 1u), theme->text_secondary);
    return;
  }
  VkrUiPanelConfig list =
      perf_grid(perf_cell(0u, row + 1u), &fill, 1u, rows, count);
  list.style.gap_pt = 2.0f;
  if (!vkr_ui_scroll_area_begin(ui, string8_lit("spikes"), &list)) {
    return;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrRecordingSpike *spike = &analysis->spikes[i];
    VkrUiWidgetConfig line = vkr_ui_widget_config_default();
    line.placement = perf_cell(0u, i);
    vkr_editor_ghost_style(&line);
    line.style.font_size_pt = theme->font_caption;
    line.style.padding_pt = (VkrUiEdges){2.0f, 8.0f, 2.0f, 8.0f};
    line.text.font = editor->mono_font;
    line.style.text_color = spike->frame_ms > 2.0 * analysis->budget_ms
                                ? theme->error
                                : theme->text;
    if (perf->selected == spike->frame) {
      line.style.background_color = theme->selection;
    }
    line.tooltip = string8_lit("Watch this frame in the Scene");
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("spike"), perf_spike_text(ui, perf, i),
                      &line)) {
      perf_seek(editor, frame, spike->frame);
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_scroll_area_end(ui);
}

void vkr_editor_recording_perf_build(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     VkrUiRect bounds) {
  (void)bounds;
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  if (!perf->loaded) {
    VkrUiPanelConfig body = perf_grid(perf_cell(0u, 0u), &fill, 1u, &fill, 1u);
    body.style.padding_pt = (VkrUiEdges){12.0f, 14.0f, 12.0f, 14.0f};
    if (vkr_ui_panel_begin(ui, string8_lit("perf.empty"), &body)) {
      VkrUiWidgetConfig text =
          vkr_editor_text_config(theme->font_body, theme->text_secondary);
      text.placement = perf_cell(0u, 0u);
      text.placement.align = VKR_UI_ALIGN_START;
      text.text.layout.word_wrap = true_v;
      vkr_ui_label(ui, string8_lit("text"),
                   string8_lit("Choose Analyze on a recording in the "
                               "Recordings window or the playback bar."),
                   &text);
      (void)vkr_ui_panel_end(ui);
    }
    return;
  }
  const VkrUiTrack rows[] = {
      {.value = 18.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 50.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 18.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 2.0f, .unit = VKR_UI_TRACK_FR},
      {.value = 18.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 1.2f, .unit = VKR_UI_TRACK_FR},
      {.value = 84.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 18.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 140.0f, .unit = VKR_UI_TRACK_PX},
  };
  VkrUiPanelConfig body =
      perf_grid(perf_cell(0u, 0u), &fill, 1u, rows, ArrayCount(rows));
  body.style.padding_pt = (VkrUiEdges){10.0f, 12.0f, 10.0f, 12.0f};
  body.style.gap_pt = theme->space_sm;
  body.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("perf.body"), &body)) {
    return;
  }
  const VkrRecordingData *data = &perf->data;
  perf_title(ui, "header",
             string8_create_formatted(
                 ui->frame_allocator,
                 "%s  \xc2\xb7  %.1f s  \xc2\xb7  %llu frames%s%s", perf->name,
                 data->times[data->frame_count - 1u],
                 (unsigned long long)data->frame_count,
                 data->missing_frames ? "  \xc2\xb7  frames lost" : "",
                 data->truncated ? "  \xc2\xb7  cut off" : ""),
             perf_cell(0u, 0u), false_v);
  perf_cards(editor, ui, 1u);
  perf_frame_chart(editor, frame, 2u);
  perf_stage_chart(editor, frame, 4u);
  perf_memory_charts(editor, frame, 6u);
  perf_spikes(editor, frame, 7u);
  (void)vkr_ui_panel_end(ui);
}
