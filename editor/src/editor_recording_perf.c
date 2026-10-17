#include "editor_recording.h"

#include "containers/vkr_sort.h"
#include "core/vkr_content_codec.h"
#include "editor_internal.h"
#include "editor_ui.h"
#include "memory/vkr_dmemory_allocator.h"

/* The Performance window (docs/proposals/recordings.md, "Performance
   view"): a recording's statistics, its frame-time, CPU-stage, pass and
   memory charts, its spikes and its passes. A click on a chart or a spike
   seeks the playback to that frame, opening the recording first; a click on
   a pass isolates it in the pass chart. */

#define PERF_MEMORY_COMMIT (1ull * 1024ull * 1024ull)
#define PERF_MEMORY_RESERVE (1024ull * 1024ull * 1024ull)
#define PERF_GB (1024.0 * 1024.0 * 1024.0)
/* pass_series entry of the series that sums every other pass. */
#define PERF_PASS_OTHER UINT32_MAX
/* A frame without GPU pass times shows the nearest one within this many
   frames: one tiled sampling period. */
#define PERF_PASS_REACH 8u
/* Rows of the pass table. */
#define PERF_PASS_ROWS 128u

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

/* Pass series colors: the stage colors, three more hues, and a neutral for
   the sum of the other passes. */
static const Vec4 PERF_PASS_COLOR[VKR_EDITOR_PERF_PASS_SERIES] = {
    {0.224f, 0.529f, 0.898f, 1.0f}, {0.851f, 0.349f, 0.149f, 1.0f},
    {0.098f, 0.620f, 0.439f, 1.0f}, {0.788f, 0.522f, 0.000f, 1.0f},
    {0.541f, 0.388f, 0.824f, 1.0f}, {0.769f, 0.271f, 0.561f, 1.0f},
    {0.184f, 0.651f, 0.722f, 1.0f}, {0.500f, 0.500f, 0.520f, 1.0f},
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

/* The series pass `pass` draws in, or UINT32_MAX when it is not shown. */
vkr_internal uint32_t perf_pass_series_of(const VkrEditorRecordingPerf *perf,
                                          uint32_t pass) {
  for (uint32_t s = 0u; s < perf->pass_series_count; ++s) {
    if (perf->pass_series[s] == pass) {
      return s;
    }
  }
  const uint32_t last = perf->pass_series_count - 1u;
  return perf->pass_series_count > 0u &&
                 perf->pass_series[last] == PERF_PASS_OTHER
             ? last
             : UINT32_MAX;
}

/* The time the pass chart shows for a row: GPU or CPU; false_v when the
   row has none. */
vkr_internal bool8_t perf_row_ms(const VkrEditorRecordingPerf *perf,
                                 const VkrRecordingPassRow *row,
                                 float32_t *out_ms) {
  if (perf->pass_gpu && !(row->flags & VKR_RECORDING_PASS_GPU_VALID)) {
    return false_v;
  }
  *out_ms = perf->pass_gpu ? row->gpu_ms : row->cpu_ms;
  return true_v;
}

/* A frame the pass chart can show: one with rows, with GPU times when the
   chart shows GPU times. */
vkr_internal bool8_t perf_pass_frame(const VkrEditorRecordingPerf *perf,
                                     uint64_t frame) {
  const VkrRecordingData *data = &perf->data;
  uint32_t count = 0u;
  (void)vkr_recording_data_pass_rows(data, frame, &count);
  return count > 0u && (!perf->pass_gpu || (data->pass_frame_flags[frame] &
                                            VKR_RECORDING_PASSES_GPU_TIMED));
}

/* The shown passes' time in `frame`. */
vkr_internal float32_t perf_pass_total(const VkrEditorRecordingPerf *perf,
                                       uint64_t frame) {
  uint32_t count = 0u;
  const VkrRecordingPassRow *rows =
      vkr_recording_data_pass_rows(&perf->data, frame, &count);
  float32_t total = 0.0f;
  for (uint32_t i = 0u; i < count; ++i) {
    float32_t ms = 0.0f;
    if (perf_pass_series_of(perf, rows[i].pass) != UINT32_MAX &&
        perf_row_ms(perf, &rows[i], &ms)) {
      total += ms;
    }
  }
  return total;
}

vkr_internal int32_t perf_compare_f32(const void *lhs, const void *rhs) {
  const float32_t a = *(const float32_t *)lhs;
  const float32_t b = *(const float32_t *)rhs;
  return a < b ? -1 : (a > b ? 1 : 0);
}

/* The pass chart's series and columns (VkrEditorRecordingPerf.has_passes). */
vkr_internal void perf_pass_columns(VkrEditorRecordingPerf *perf) {
  const VkrRecordingData *data = &perf->data;
  const VkrRecordingAnalysis *analysis = &perf->analysis;
  perf->has_passes = data->pass_frames > 0u && analysis->pass_count > 0u;
  perf->pass_column_count = 0u;
  if (!perf->has_passes) {
    return;
  }
  perf->pass_gpu = data->pass_gpu_frames > 0u;
  perf->pass_series_count = 0u;
  if (perf->pass_focus != UINT32_MAX) {
    perf->pass_series[perf->pass_series_count++] = perf->pass_focus;
  } else {
    const uint32_t named =
        Min(analysis->pass_count, VKR_EDITOR_PERF_PASS_SERIES - 1u);
    for (uint32_t i = 0u; i < named; ++i) {
      perf->pass_series[perf->pass_series_count++] = analysis->passes[i].pass;
    }
    if (analysis->pass_count > named) {
      perf->pass_series[perf->pass_series_count++] = PERF_PASS_OTHER;
    }
  }

  const uint64_t eligible =
      perf->pass_gpu ? data->pass_gpu_frames : data->pass_frames;
  const uint32_t columns =
      (uint32_t)Min(eligible, (uint64_t)VKR_EDITOR_PERF_COLUMNS);
  float32_t totals[VKR_EDITOR_PERF_COLUMNS];
  uint64_t k = 0u;
  for (uint64_t f = 0u; f < data->frame_count; ++f) {
    if (!perf_pass_frame(perf, f)) {
      continue;
    }
    const uint32_t c = (uint32_t)(k * columns / eligible);
    const float32_t total = perf_pass_total(perf, f);
    if (k * columns % eligible < columns || total > totals[c]) {
      totals[c] = total;
      perf->pass_column_frame[c] = f;
    }
    ++k;
  }

  for (uint32_t s = 0u; s < perf->pass_series_count; ++s) {
    MemZero(perf->pass_ms[s], sizeof(float32_t) * columns);
  }
  for (uint32_t c = 0u; c < columns; ++c) {
    uint32_t count = 0u;
    const VkrRecordingPassRow *rows =
        vkr_recording_data_pass_rows(data, perf->pass_column_frame[c], &count);
    for (uint32_t i = 0u; i < count; ++i) {
      const uint32_t series = perf_pass_series_of(perf, rows[i].pass);
      float32_t ms = 0.0f;
      if (series != UINT32_MAX && perf_row_ms(perf, &rows[i], &ms)) {
        perf->pass_ms[series][c] += ms;
      }
    }
  }
  perf->pass_column_count = columns;
  /* As for frame times, one giant column must not flatten the rest. */
  vkr_sort(totals, columns, sizeof(float32_t), perf_compare_f32);
  const float32_t max = columns > 0u ? totals[columns - 1u] : 0.0f;
  const float32_t p99 =
      columns > 0u ? totals[Min((columns * 99u + 99u) / 100u, columns) - 1u]
                   : 0.0f;
  perf->pass_ms_top = Max(Min(max * 1.05f, p99 * 3.0f), 0.5f);
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
  perf->pass_focus = UINT32_MAX;
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
  perf_pass_columns(perf);
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

/* The pass chart column nearest `frame`, or VKR_UI_PLOT_NONE. Columns are
   in frame order. */
vkr_internal uint32_t perf_pass_column_of(const VkrEditorRecordingPerf *perf,
                                          uint64_t frame) {
  const uint32_t count = perf->pass_column_count;
  if (frame == UINT64_MAX || count == 0u) {
    return VKR_UI_PLOT_NONE;
  }
  uint32_t low = 0u;
  uint32_t high = count;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2u;
    if (perf->pass_column_frame[middle] < frame) {
      low = middle + 1u;
    } else {
      high = middle;
    }
  }
  if (low == count) {
    return count - 1u;
  }
  if (low > 0u && frame - perf->pass_column_frame[low - 1u] <
                      perf->pass_column_frame[low] - frame) {
    return low - 1u;
  }
  return low;
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

/* The tooltip of a hovered pass column: the frame, its pass time and its
   slowest shown passes. */
vkr_internal String8 perf_pass_tooltip(VkrUiSystem *ui,
                                       const VkrEditorRecordingPerf *perf,
                                       uint32_t column) {
  if (column >= perf->pass_column_count) {
    return (String8){0};
  }
  const VkrRecordingData *data = &perf->data;
  const uint64_t f = perf->pass_column_frame[column];
  String8 text = string8_create_formatted(
      ui->frame_allocator,
      "Frame %llu  \xc2\xb7  %.3f s  \xc2\xb7  passes %.2f ms %s",
      (unsigned long long)f, data->times[f], perf_pass_total(perf, f),
      perf->pass_gpu ? "GPU" : "CPU");
  uint32_t count = 0u;
  const VkrRecordingPassRow *rows =
      vkr_recording_data_pass_rows(data, f, &count);
  /* The four slowest rows, by selection. */
  uint32_t top[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
  float32_t top_ms[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
  for (uint32_t i = 0u; i < count; ++i) {
    float32_t ms = 0.0f;
    if (perf_pass_series_of(perf, rows[i].pass) == UINT32_MAX ||
        !perf_row_ms(perf, &rows[i], &ms)) {
      continue;
    }
    uint32_t candidate = i;
    for (uint32_t rank = 0u; rank < ArrayCount(top); ++rank) {
      if (ms > top_ms[rank]) {
        const uint32_t displaced = top[rank];
        const float32_t displaced_ms = top_ms[rank];
        top[rank] = candidate;
        top_ms[rank] = ms;
        candidate = displaced;
        ms = displaced_ms;
        if (candidate == UINT32_MAX) {
          break;
        }
      }
    }
  }
  for (uint32_t rank = 0u; rank < ArrayCount(top); ++rank) {
    if (top[rank] == UINT32_MAX) {
      break;
    }
    text = string8_create_formatted(
        ui->frame_allocator, "%.*s\n%s  %.2f ms", (int32_t)text.length,
        text.str, data->passes[rows[top[rank]].pass].name, top_ms[rank]);
  }
  return string8_create_formatted(ui->frame_allocator, "%.*s\nClick to watch",
                                  (int32_t)text.length, text.str);
}

/* A plot that seeks on click and describes the hovered column; `passes`
   selects the pass chart's columns. */
vkr_internal void perf_plot(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                            const char *id, VkrUiPlacement placement,
                            VkrUiPlotConfig *plot, bool8_t passes) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrRecordingPlayback *playback = perf_playback(perf, frame);
  const uint64_t shown =
      playback ? vkr_recording_playback_frame_at(playback, playback->time)
               : perf->selected;
  plot->cursor =
      passes ? perf_pass_column_of(perf, shown) : perf_column_of(perf, shown);
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
  uint32_t *hovered =
      passes ? &perf->pass_hovered_column : &perf->hovered_column;
  config.tooltip = passes ? perf_pass_tooltip(ui, perf, *hovered)
                          : perf_tooltip(ui, perf, *hovered);
  const VkrUiPlotResult result = vkr_ui_plot(
      ui, string8_create_from_cstr((const uint8_t *)id, string_length(id)),
      plot, &config);
  if (result.hovered != VKR_UI_PLOT_NONE) {
    *hovered = result.hovered;
  }
  const uint32_t column_count =
      passes ? perf->pass_column_count : perf->column_count;
  const uint64_t *column_frame =
      passes ? perf->pass_column_frame : perf->column_frame;
  if (result.clicked != VKR_UI_PLOT_NONE && result.clicked < column_count) {
    perf_seek(editor, frame, column_frame[result.clicked]);
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
    key.icon = VKR_UI_ICON_DOT;
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
  perf_plot(editor, frame, "frame.plot", perf_cell(0u, row + 1u), &plot,
            false_v);
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
  perf_plot(editor, frame, "stage.plot", perf_cell(0u, row + 1u), &plot,
            false_v);
}

/* The color of pass `pass`: its series color among the slowest passes, else
   the color of the rest. */
vkr_internal Vec4 perf_pass_color(const VkrEditorRecordingPerf *perf,
                                  uint32_t pass) {
  const uint32_t named =
      Min(perf->analysis.pass_count, VKR_EDITOR_PERF_PASS_SERIES - 1u);
  for (uint32_t i = 0u; i < named; ++i) {
    if (perf->analysis.passes[i].pass == pass) {
      return PERF_PASS_COLOR[i];
    }
  }
  return PERF_PASS_COLOR[VKR_EDITOR_PERF_PASS_SERIES - 1u];
}

/* What the pass chart shows and how often the frames took GPU stamps. */
vkr_internal String8 perf_pass_title(VkrUiSystem *ui,
                                     const VkrEditorRecordingPerf *perf) {
  const VkrRecordingData *data = &perf->data;
  if (!perf->has_passes) {
    return string8_lit("Passes: not recorded. Tier 1 recordings, which the "
                       "Record button makes, carry pass rows.");
  }
  const char *time = perf->pass_gpu ? "GPU" : "CPU encode";
  if (perf->pass_focus != UINT32_MAX) {
    return string8_create_formatted(
        ui->frame_allocator,
        "%s of %s (ms)  \xc2\xb7  click the pass again to show every pass",
        time, data->passes[perf->pass_focus].name);
  }
  if (!perf->pass_gpu) {
    return string8_lit("CPU encode by pass (ms), stacked  \xc2\xb7  no GPU "
                       "pass times in this recording");
  }
  if (data->pass_gpu_frames == data->pass_frames) {
    return string8_lit("GPU by pass (ms), stacked  \xc2\xb7  every frame");
  }
  return string8_create_formatted(
      ui->frame_allocator,
      "GPU by pass (ms), stacked  \xc2\xb7  sampled in %llu of %llu frames; "
      "sampled frames pay for the stamps",
      (unsigned long long)data->pass_gpu_frames,
      (unsigned long long)data->pass_frames);
}

vkr_internal void perf_pass_chart(VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame, uint32_t row) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  perf_title(frame->ui, "pass.title", perf_pass_title(frame->ui, perf),
             perf_cell(0u, row), false_v);
  if (!perf->has_passes) {
    return;
  }
  VkrUiPlotSeries bars[VKR_EDITOR_PERF_PASS_SERIES];
  for (uint32_t s = 0u; s < perf->pass_series_count; ++s) {
    const uint32_t pass = perf->pass_series[s];
    bars[s] = (VkrUiPlotSeries){
        .values = perf->pass_ms[s],
        .color = pass == PERF_PASS_OTHER
                     ? PERF_PASS_COLOR[VKR_EDITOR_PERF_PASS_SERIES - 1u]
                     : perf_pass_color(perf, pass),
    };
  }
  VkrUiPlotConfig plot = {
      .sample_count = perf->pass_column_count,
      .y_min = 0.0f,
      .y_max = perf->pass_ms_top,
      .bars = bars,
      .bar_count = perf->pass_series_count,
  };
  perf_plot(editor, frame, "pass.plot", perf_cell(0u, row + 1u), &plot, true_v);
}

/* The frame the pass table's last column reads: the playback's or the
   selected frame, or the nearest one with GPU times; UINT64_MAX for none. */
vkr_internal uint64_t perf_pass_table_frame(const VkrEditorRecordingPerf *perf,
                                            const VkrSampleUiFrame *frame) {
  const VkrRecordingPlayback *playback = perf_playback(perf, frame);
  const uint64_t at =
      playback ? vkr_recording_playback_frame_at(playback, playback->time)
               : perf->selected;
  if (at == UINT64_MAX || !perf->pass_gpu) {
    return at;
  }
  return vkr_recording_data_gpu_frame_near(&perf->data, at, PERF_PASS_REACH);
}

/* The chart's time a pass took over the whole recording. */
vkr_internal float64_t perf_pass_sum(const VkrEditorRecordingPerf *perf,
                                     const VkrRecordingPassStats *stats) {
  const VkrRecordingStats *shown = perf->pass_gpu ? &stats->gpu : &stats->cpu;
  return shown->mean * (float64_t)shown->valid;
}

/* One pass table row: name, share of all pass time, mean, p95 and max of
   the chart's time when the pass ran, CPU encode mean and the time at the
   table's frame. */
vkr_internal String8 perf_pass_text(VkrUiSystem *ui,
                                    const VkrEditorRecordingPerf *perf,
                                    const VkrRecordingPassStats *stats,
                                    float64_t all_ms, bool8_t at_valid,
                                    float32_t at_ms) {
  const VkrRecordingStats *shown = perf->pass_gpu ? &stats->gpu : &stats->cpu;
  char at[16] = "      -";
  if (at_valid) {
    string_format(at, sizeof(at), "%7.2f", at_ms);
  }
  if (shown->valid == 0u) {
    return string8_create_formatted(ui->frame_allocator,
                                    "%-30.30s %6s %7s %7s %7s %7.3f %s",
                                    perf->data.passes[stats->pass].name, "-",
                                    "-", "-", "-", stats->cpu.mean, at);
  }
  const float64_t share =
      all_ms > 0.0 ? 100.0 * perf_pass_sum(perf, stats) / all_ms : 0.0;
  return string8_create_formatted(
      ui->frame_allocator, "%-30.30s %5.1f%% %7.2f %7.2f %7.2f %7.3f %s",
      perf->data.passes[stats->pass].name, share, shown->mean, shown->p95,
      shown->max, stats->cpu.mean, at);
}

/* The passes, most total time first; a click isolates one in the pass
   chart. */
vkr_internal void perf_pass_table(VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame,
                                  uint32_t column) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrRecordingData *data = &perf->data;
  const VkrRecordingAnalysis *analysis = &perf->analysis;
  if (!perf->has_passes) {
    perf_title(ui, "passes.title", string8_lit("Passes: not recorded"),
               perf_cell(column, 0u), false_v);
    return;
  }
  perf_title(ui, "passes.title",
             string8_create_formatted(
                 ui->frame_allocator,
                 "Passes: %u, by total time  \xc2\xb7  %s ms when they ran, "
                 "over %llu frames; click one to "
                 "isolate it",
                 analysis->pass_count, perf->pass_gpu ? "GPU" : "CPU encode",
                 (unsigned long long)(perf->pass_gpu ? data->pass_gpu_frames
                                                     : data->pass_frames)),
             perf_cell(column, 0u), false_v);

  /* The table frame's times, by pass id. */
  const uint64_t at = perf_pass_table_frame(perf, frame);
  float32_t at_ms[VKR_RECORDING_PASS_MAX];
  uint8_t at_valid[VKR_RECORDING_PASS_MAX] = {0};
  uint32_t at_count = 0u;
  const VkrRecordingPassRow *at_rows =
      vkr_recording_data_pass_rows(data, at, &at_count);
  for (uint32_t i = 0u; i < at_count; ++i) {
    if (at_rows[i].pass < VKR_RECORDING_PASS_MAX &&
        perf_row_ms(perf, &at_rows[i], &at_ms[at_rows[i].pass])) {
      at_valid[at_rows[i].pass] = 1u;
    }
  }

  const uint32_t count = Min(analysis->pass_count, (uint32_t)PERF_PASS_ROWS);
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiTrack rows[PERF_PASS_ROWS + 1u];
  for (uint32_t i = 0u; i <= count; ++i) {
    rows[i] = (VkrUiTrack){.value = 20.0f, .unit = VKR_UI_TRACK_PX};
  }
  VkrUiPanelConfig list =
      perf_grid(perf_cell(column, 1u), &fill, 1u, rows, count + 1u);
  list.style.gap_pt = 1.0f;
  if (!vkr_ui_scroll_area_begin(ui, string8_lit("passes"), &list)) {
    return;
  }
  char at_title[24] = "  @frame";
  if (at != UINT64_MAX) {
    string_format(at_title, sizeof(at_title), "  @%llu",
                  (unsigned long long)at);
  }
  VkrUiWidgetConfig header =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  header.placement = perf_cell(0u, 0u);
  header.placement.justify = VKR_UI_ALIGN_START;
  header.placement.align = VKR_UI_ALIGN_CENTER;
  header.style.padding_pt = (VkrUiEdges){0.0f, 8.0f, 0.0f, 26.0f};
  header.text.font = editor->mono_font;
  vkr_ui_label(ui, string8_lit("header"),
               string8_create_formatted(
                   ui->frame_allocator, "%-30s %6s %7s %7s %7s %7s %s", "Pass",
                   "share", "mean", "p95", "max", "CPU", at_title),
               &header);
  float64_t all_ms = 0.0;
  for (uint32_t i = 0u; i < analysis->pass_count; ++i) {
    all_ms += perf_pass_sum(perf, &analysis->passes[i]);
  }
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrRecordingPassStats *stats = &analysis->passes[i];
    VkrUiWidgetConfig line = vkr_ui_widget_config_default();
    line.placement = perf_cell(0u, i + 1u);
    vkr_editor_ghost_style(&line);
    line.style.font_size_pt = theme->font_caption;
    line.style.padding_pt = (VkrUiEdges){2.0f, 8.0f, 2.0f, 8.0f};
    line.text.font = editor->mono_font;
    line.icon = VKR_UI_ICON_DOT;
    line.icon_size_pt = 10.0f;
    line.icon_color = perf_pass_color(perf, stats->pass);
    if (perf->pass_focus == stats->pass) {
      line.style.background_color = theme->selection;
    }
    line.tooltip = perf->pass_focus == stats->pass
                       ? string8_lit("Show every pass in the chart")
                       : string8_lit("Show only this pass in the chart");
    (void)vkr_ui_push_id_u64(ui, stats->pass);
    if (vkr_ui_button(ui, string8_lit("pass"),
                      perf_pass_text(ui, perf, stats, all_ms,
                                     at_valid[stats->pass], at_ms[stats->pass]),
                      &line)) {
      perf->pass_focus =
          perf->pass_focus == stats->pass ? UINT32_MAX : stats->pass;
      perf_pass_columns(perf);
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_scroll_area_end(ui);
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
    perf_plot(editor, frame, "plot", perf_cell(0u, 1u), &plot, false_v);
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
  /* CPU encode times barely move at a spike; only GPU pass times explain
     one on this line. */
  char pass[112] = "";
  if (spike->pass_gpu && spike->pass_contributor_count > 0u) {
    const VkrRecordingPassContributor *top = &spike->passes[0];
    string_format(pass, sizeof(pass), "  %s GPU +%.2f ms",
                  perf->data.passes[top->pass].name, top->excess);
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
      "#%-3u frame %-7llu %8.2f s %8.2f ms  x%-5.1f %s%s%s%s", index + 1u,
      (unsigned long long)spike->frame, spike->time, spike->frame_ms,
      spike->ratio, cause, pass, marker[0] ? "  \xe2\x9a\x91 " : "", marker);
}

vkr_internal void perf_spikes(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame, uint32_t column) {
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
             perf_cell(column, 0u), false_v);
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
                 perf_cell(column, 1u), theme->text_secondary);
    return;
  }
  VkrUiPanelConfig list =
      perf_grid(perf_cell(column, 1u), &fill, 1u, rows, count);
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
  const VkrUiTrack title = {.value = 18.0f, .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack rows[] = {
      title,
      {.value = 50.0f, .unit = VKR_UI_TRACK_PX},
      title,
      {.value = 2.0f, .unit = VKR_UI_TRACK_FR},
      title,
      {.value = 1.2f, .unit = VKR_UI_TRACK_FR},
      title,
      {.value = perf->has_passes ? 1.4f : 0.0f, .unit = VKR_UI_TRACK_FR},
      {.value = 84.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 200.0f, .unit = VKR_UI_TRACK_PX},
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
  perf_pass_chart(editor, frame, 6u);
  perf_memory_charts(editor, frame, 8u);
  /* Spikes and passes side by side, each a title over a list. */
  const VkrUiTrack columns[] = {{.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                                {.value = 1.0f, .unit = VKR_UI_TRACK_FR}};
  const VkrUiTrack lists[] = {title, fill};
  VkrUiPanelConfig bottom =
      perf_grid(perf_cell(0u, 9u), columns, ArrayCount(columns), lists,
                ArrayCount(lists));
  bottom.style.gap_pt = theme->space_md;
  if (vkr_ui_panel_begin(ui, string8_lit("perf.lists"), &bottom)) {
    perf_spikes(editor, frame, 0u);
    perf_pass_table(editor, frame, 1u);
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_panel_end(ui);
}
