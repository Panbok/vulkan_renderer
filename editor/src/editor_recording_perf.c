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

void vkr_editor_recording_compare_stop(VkrEditorRecordingPerf *perf) {
  if (perf->comparing) {
    vkr_recording_compare_free(&perf->compare);
    vkr_recording_analysis_free(&perf->compare_analysis);
    vkr_recording_data_free(&perf->compare_data);
  }
  perf->comparing = false_v;
}

void vkr_editor_recording_perf_release(VkrEditorRecordingPerf *perf) {
  vkr_editor_recording_compare_stop(perf);
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

/* The bundle's name, without its directory and extension. */
vkr_internal void perf_bundle_name(const char *path, char *out,
                                   uint64_t capacity) {
  const char *base = path;
  for (const char *c = path; *c; ++c) {
    if (*c == '/' || *c == '\\') {
      base = c + 1;
    }
  }
  string_format(out, capacity, "%s", base);
  const uint64_t length = string_length(out);
  const uint64_t extension = sizeof(VKR_RECORDING_BUNDLE_EXTENSION) - 1u;
  if (length > extension &&
      string_equals(out + length - extension, VKR_RECORDING_BUNDLE_EXTENSION)) {
    out[length - extension] = '\0';
  }
}

/* B's frame time at each column's frame, holding the last one where B has
   none at that frame; the chart's top grows to hold it. */
vkr_internal void perf_compare_columns(VkrEditorRecordingPerf *perf) {
  float32_t held = 0.0f;
  for (uint32_t c = 0u; c < perf->column_count; ++c) {
    const uint64_t frame = perf->column_frame[c];
    const uint64_t match = frame < perf->data.frame_count
                               ? perf->compare.b_frame[frame]
                               : UINT64_MAX;
    float64_t ms = 0.0;
    if (match != UINT64_MAX &&
        vkr_recording_data_frame_ms(&perf->compare_data, match, &ms)) {
      held = (float32_t)ms;
    }
    perf->compare_ms[c] = held;
    perf->frame_ms_top = Max(perf->frame_ms_top, held * 1.05f);
  }
}

bool8_t vkr_editor_recording_compare(VkrEditorUi *editor, const char *name,
                                     VkrRecordingAlign align,
                                     const char *marker, char *error,
                                     uint64_t error_capacity) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  if (!perf->loaded) {
    string_format(error, error_capacity,
                  "Analyze a recording first; it is what B is compared with.");
    return false_v;
  }
  char path[VKR_RECORDING_PATH_MAX];
  if (!vkr_editor_recording_resolve(editor, name, path, sizeof(path))) {
    string_format(error, error_capacity, "No recording named '%s'.",
                  name ? name : "");
    return false_v;
  }
  /* B stays loaded while only the alignment changes. */
  const bool8_t same =
      perf->comparing && string_equals(perf->compare_path, path);
  VkrRecordingData data = perf->compare_data;
  VkrRecordingAnalysis analysis = perf->compare_analysis;
  if (!same) {
    if (vkr_recording_data_load(&data, &perf->allocator, path,
                                vkr_content_codec_zstd_decoder()) !=
            VKR_RECORDING_DATA_OK ||
        data.frame_count == 0u) {
      string_format(error, error_capacity, "'%s' is not a readable recording.",
                    path);
      return false_v;
    }
    if (!vkr_recording_analyze(&data, NULL, &perf->allocator, &analysis)) {
      vkr_recording_data_free(&data);
      string_format(error, error_capacity,
                    "The recording could not be analyzed.");
      return false_v;
    }
  }
  const VkrRecordingCompareOptions options = {.align = align, .marker = marker};
  VkrRecordingCompare compare;
  if (!vkr_recording_compare(&perf->data, &perf->analysis, &data, &analysis,
                             &options, &perf->allocator, &compare, error,
                             error_capacity)) {
    if (!same) {
      vkr_recording_analysis_free(&analysis);
      vkr_recording_data_free(&data);
    }
    return false_v;
  }
  if (same) {
    vkr_recording_compare_free(&perf->compare);
  } else {
    vkr_editor_recording_compare_stop(perf);
    perf->compare_data = data;
    perf->compare_analysis = analysis;
    string_format(perf->compare_path, sizeof(perf->compare_path), "%s", path);
    perf_bundle_name(path, perf->compare_name, sizeof(perf->compare_name));
  }
  perf->compare = compare;
  perf->comparing = true_v;
  perf_columns(perf);
  perf_compare_columns(perf);
  vkr_editor_window_set_visible(editor, VKR_EDITOR_WINDOW_PERFORMANCE, true_v);
  return true_v;
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
  const VkrEditorRecordingPerfView view = perf->view;
  MemZero(perf, sizeof(*perf));
  perf->view = view;
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
  perf_bundle_name(path, perf->name, sizeof(perf->name));
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
/* Recording B's line over A's bars. */
static const Vec4 PERF_COMPARE_COLOR = {0.690f, 0.553f, 0.984f, 1.0f};
/* A change smaller than this share of A's value reads as no change. */
#define PERF_COMPARE_SAME 0.02

/* The colour of B - A for a statistic where lower is better. */
vkr_internal Vec4 perf_compare_color(float64_t a, float64_t b) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const float64_t threshold = Max(vkr_abs_f64(a) * PERF_COMPARE_SAME, 1.0e-6);
  return b - a > threshold   ? theme->error
         : a - b > threshold ? theme->success
                             : theme->text_secondary;
}

/* B's value of card `id` and its change, as the card's last line. */
vkr_internal String8 perf_compare_line(VkrUiSystem *ui,
                                       const VkrEditorRecordingPerf *perf,
                                       VkrRecordingCompareStatId id,
                                       float64_t scale, const char *unit) {
  const VkrRecordingCompareStat *stat = &perf->compare.stats[id];
  if (!stat->valid) {
    return string8_lit("B: not measured");
  }
  const float64_t a = stat->a * scale;
  const float64_t b = stat->b * scale;
  /* B's value and the change as a share of A's; counts and a zero A take
     the plain difference. */
  if (id == VKR_RECORDING_COMPARE_OVER_BUDGET) {
    return string8_create_formatted(ui->frame_allocator, "B %.0f  (%+.0f)", b,
                                    b - a);
  }
  if (a == 0.0) {
    return string8_create_formatted(ui->frame_allocator, "B %.2f%s  (%+.2f)", b,
                                    unit, b - a);
  }
  return string8_create_formatted(ui->frame_allocator, "B %.2f%s  (%+.0f%%)", b,
                                  unit, 100.0 * (b - a) / a);
}

/* One statistic: its caption, A's value and, while comparing, B's value
   and the change in `compare_color`. */
vkr_internal void perf_card(VkrEditorUi *editor, VkrUiSystem *ui,
                            uint32_t column, const char *caption, String8 value,
                            Vec4 color, String8 compare, Vec4 compare_color) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {{.value = 14.0f, .unit = VKR_UI_TRACK_PX},
                             {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                             {.value = 14.0f, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig card = perf_grid(perf_cell(column, 0u), &fill, 1u, rows,
                                    compare.length ? 3u : 2u);
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
    if (compare.length) {
      perf_caption(ui, "compare", compare, perf_cell(0u, 2u), compare_color);
    }
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_pop_id(ui);
}

/* A statistic's card; while comparing, its last line is B's value of `id`
   in `unit` after `scale`, and the change. */
vkr_internal void perf_stat_card(VkrEditorUi *editor, VkrUiSystem *ui,
                                 uint32_t column, const char *caption,
                                 String8 value, Vec4 color,
                                 VkrRecordingCompareStatId id, float64_t scale,
                                 const char *unit) {
  const VkrEditorRecordingPerf *perf = &editor->recording.perf;
  String8 line = {0};
  Vec4 line_color = vkr_ui_theme()->text_secondary;
  if (perf->comparing) {
    const VkrRecordingCompareStat *stat = &perf->compare.stats[id];
    line = perf_compare_line(ui, perf, id, scale, unit);
    if (stat->valid) {
      line_color = perf_compare_color(stat->a, stat->b);
    }
  }
  perf_card(editor, ui, column, caption, value, color, line, line_color);
}

vkr_internal String8 perf_gb(VkrUiSystem *ui, const VkrRecordingData *data,
                             const VkrRecordingAnalysis *analysis,
                             const char *series) {
  const uint32_t index = vkr_recording_data_find_series(data, series);
  if (index == UINT32_MAX || analysis->series[index].valid == 0u) {
    return string8_lit("n/a");
  }
  const VkrRecordingStats *stats = &analysis->series[index];
  return string8_create_formatted(ui->frame_allocator, "%.2f / %.2f GiB",
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
  perf_stat_card(
      editor, ui, 0u, "p50",
      string8_create_formatted(ui->frame_allocator, "%.2f ms", time->p50),
      theme->text, VKR_RECORDING_COMPARE_P50, 1.0, "");
  perf_stat_card(
      editor, ui, 1u, "p95",
      string8_create_formatted(ui->frame_allocator, "%.2f ms", time->p95),
      theme->text, VKR_RECORDING_COMPARE_P95, 1.0, "");
  perf_stat_card(
      editor, ui, 2u, "p99",
      string8_create_formatted(ui->frame_allocator, "%.2f ms", time->p99),
      p99_color, VKR_RECORDING_COMPARE_P99, 1.0, "");
  perf_stat_card(
      editor, ui, 3u, "max",
      string8_create_formatted(ui->frame_allocator, "%.2f ms", time->max),
      max_color, VKR_RECORDING_COMPARE_MAX, 1.0, "");
  perf_stat_card(editor, ui, 4u, "1% low",
                 string8_create_formatted(ui->frame_allocator, "%.2f ms",
                                          analysis->low_1pct_ms),
                 theme->text, VKR_RECORDING_COMPARE_LOW_1PCT, 1.0, "");
  perf_stat_card(
      editor, ui, 5u, "over 16.7 ms",
      string8_create_formatted(ui->frame_allocator, "%llu frame%s",
                               (unsigned long long)analysis->over_budget,
                               analysis->over_budget == 1u ? "" : "s"),
      analysis->over_budget ? theme->warning : theme->text,
      VKR_RECORDING_COMPARE_OVER_BUDGET, 1.0, "");
  perf_stat_card(editor, ui, 6u, "RAM peak / avg",
                 perf_gb(ui, &perf->data, analysis, "process.resident_bytes"),
                 theme->text, VKR_RECORDING_COMPARE_RAM_PEAK, 1.0 / PERF_GB,
                 " GiB");
  perf_stat_card(editor, ui, 7u, "VRAM peak / avg",
                 perf_gb(ui, &perf->data, analysis,
                         "memory.gpu.driver.current_allocated_bytes"),
                 theme->text, VKR_RECORDING_COMPARE_VRAM_PEAK, 1.0 / PERF_GB,
                 " GiB");
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
        ui->frame_allocator, "%.*s\nRAM %.2f GiB  \xc2\xb7  VRAM %.2f GiB",
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
  perf_title(
      frame->ui, "frame.title",
      string8_create_formatted(
          frame->ui->frame_allocator,
          "Frame time (ms, %s)  \xc2\xb7  dashed at %.1f ms and "
          "twice it; green within, amber up to twice, red beyond%s%s",
          vkr_recording_data_frame_source(&perf->data), budget,
          perf->comparing ? "  \xc2\xb7  line: B aligned by " : "",
          perf->comparing ? vkr_recording_align_name(perf->compare.align) : ""),
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
  /* While comparing, B's frame times run over A's bars. */
  const VkrUiPlotSeries compare = {.values = perf->compare_ms,
                                   .color = PERF_COMPARE_COLOR};
  VkrUiPlotConfig plot = {
      .sample_count = perf->column_count,
      .y_min = 0.0f,
      .y_max = perf->frame_ms_top,
      .bars = &bars,
      .bar_count = 1u,
      .lines = perf->comparing ? &compare : NULL,
      .line_count = perf->comparing ? 1u : 0u,
      .line_width_pt = 1.5f,
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

/* A row of a sorted list: its index in the caller's order, and the value
   or name it sorts by. */
typedef struct PerfSortItem {
  float64_t key;
  const char *name;
  uint32_t index;
} PerfSortItem;

vkr_internal int32_t perf_compare_items(const void *lhs, const void *rhs) {
  const PerfSortItem *a = lhs;
  const PerfSortItem *b = rhs;
  if (a->name && b->name) {
    const int32_t order = string_compare(a->name, b->name);
    if (order != 0) {
      return order;
    }
  } else if (a->key != b->key) {
    return a->key < b->key ? -1 : 1;
  }
  return a->index < b->index ? -1 : (a->index > b->index ? 1 : 0);
}

/* Sorts `items` by the table's column, keeping the given order without
   one; descending reverses it. */
vkr_internal void perf_sort_items(const VkrEditorTable *table,
                                  PerfSortItem *items, uint32_t count) {
  if (table->sort_column == VKR_EDITOR_TABLE_NONE) {
    return;
  }
  vkr_sort(items, count, sizeof(*items), perf_compare_items);
  if (table->sort_descending) {
    for (uint32_t i = 0u; i < count / 2u; ++i) {
      const PerfSortItem swap = items[i];
      items[i] = items[count - 1u - i];
      items[count - 1u - i] = swap;
    }
  }
}

/* Pass table columns: name, share of all pass time, mean, p95 and max of
   the chart's time when the pass ran, CPU encode mean, and the time at the
   table's frame. */
enum {
  PERF_PASS_COLUMN_NAME = 0,
  PERF_PASS_COLUMN_SHARE,
  PERF_PASS_COLUMN_MEAN,
  PERF_PASS_COLUMN_P95,
  PERF_PASS_COLUMN_MAX,
  PERF_PASS_COLUMN_CPU,
  PERF_PASS_COLUMN_AT,
  PERF_PASS_COLUMN_COUNT,
};

/* The value pass `stats` sorts by in `column`; NaN-free, with passes that
   never ran last. */
vkr_internal float64_t perf_pass_key(const VkrEditorRecordingPerf *perf,
                                     const VkrRecordingPassStats *stats,
                                     uint32_t column, float64_t all_ms,
                                     bool8_t at_valid, float32_t at_ms) {
  const VkrRecordingStats *shown = perf->pass_gpu ? &stats->gpu : &stats->cpu;
  const bool8_t ran = shown->valid > 0u;
  switch (column) {
  case PERF_PASS_COLUMN_SHARE:
    return ran && all_ms > 0.0 ? perf_pass_sum(perf, stats) / all_ms : -1.0;
  case PERF_PASS_COLUMN_MEAN:
    return ran ? shown->mean : -1.0;
  case PERF_PASS_COLUMN_P95:
    return ran ? shown->p95 : -1.0;
  case PERF_PASS_COLUMN_MAX:
    return ran ? shown->max : -1.0;
  case PERF_PASS_COLUMN_CPU:
    return stats->cpu.mean;
  case PERF_PASS_COLUMN_AT:
    return at_valid ? (float64_t)at_ms : -1.0;
  default:
    return 0.0;
  }
}

/* One pass row's cells. */
vkr_internal void perf_pass_cells(VkrUiSystem *ui,
                                  const VkrEditorRecordingPerf *perf,
                                  const VkrRecordingPassStats *stats,
                                  float64_t all_ms, bool8_t at_valid,
                                  float32_t at_ms, String8 *cells) {
  const VkrRecordingStats *shown = perf->pass_gpu ? &stats->gpu : &stats->cpu;
  const bool8_t ran = shown->valid > 0u;
  VkrAllocator *frame = ui->frame_allocator;
  cells[PERF_PASS_COLUMN_NAME] = string8_create_formatted(
      frame, "%s", perf->data.passes[stats->pass].name);
  const String8 dash = string8_lit("\xe2\x80\x94");
  cells[PERF_PASS_COLUMN_SHARE] =
      ran && all_ms > 0.0
          ? string8_create_formatted(
                frame, "%.1f%%", 100.0 * perf_pass_sum(perf, stats) / all_ms)
          : dash;
  cells[PERF_PASS_COLUMN_MEAN] =
      ran ? string8_create_formatted(frame, "%.2f", shown->mean) : dash;
  cells[PERF_PASS_COLUMN_P95] =
      ran ? string8_create_formatted(frame, "%.2f", shown->p95) : dash;
  cells[PERF_PASS_COLUMN_MAX] =
      ran ? string8_create_formatted(frame, "%.2f", shown->max) : dash;
  cells[PERF_PASS_COLUMN_CPU] =
      string8_create_formatted(frame, "%.3f", stats->cpu.mean);
  cells[PERF_PASS_COLUMN_AT] =
      at_valid ? string8_create_formatted(frame, "%.2f", at_ms) : dash;
}

/* The right list's title row: its caption, and the switch between the
   passes and every metrics series. */
vkr_internal void perf_list_title(VkrEditorUi *editor, VkrUiSystem *ui,
                                  String8 text, uint32_t column) {
  VkrEditorRecordingPerfView *view = &editor->recording.perf.view;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack search_track = {.value = 180.0f, .unit = VKR_UI_TRACK_PX};
  /* The series list has a search field between the caption and switch. */
  const uint32_t search = view->show_series ? 1u : 0u;
  const VkrUiTrack columns[] = {fill, search_track, auto_track, auto_track};
  const VkrUiTrack passes_columns[] = {fill, auto_track, auto_track};
  VkrUiPanelConfig row = search
                             ? perf_grid(perf_cell(column, 0u), columns,
                                         ArrayCount(columns), &fill, 1u)
                             : perf_grid(perf_cell(column, 0u), passes_columns,
                                         ArrayCount(passes_columns), &fill, 1u);
  row.style.gap_pt = theme->space_xs;
  if (!vkr_ui_panel_begin(ui, string8_lit("list.title"), &row)) {
    return;
  }
  perf_caption(ui, "caption", text, perf_cell(0u, 0u), theme->text_secondary);
  if (search) {
    VkrUiTextEditBuffer edit = {view->series_search, view->series_search_length,
                                sizeof(view->series_search)};
    VkrUiPlacement field = perf_cell(1u, 0u);
    field.align = VKR_UI_ALIGN_CENTER;
    if (vkr_editor_search_field(ui, string8_lit("search"), &edit, field,
                                string8_lit("Filter series"),
                                string8_lit("Show the series whose name holds "
                                            "this text (case-insensitive)"),
                                VKR_FONT_HANDLE_INVALID)) {
      view->series_search_length = edit.length;
      view->series.scroll_pt = 0.0f;
    }
  }
  static const char *const labels[] = {"Passes", "Series"};
  static const char *const tips[] = {
      "Render passes and their times",
      "Every metrics series the recording carries, with its statistics",
  };
  for (uint32_t i = 0u; i < ArrayCount(labels); ++i) {
    VkrUiWidgetConfig mode = vkr_ui_widget_config_default();
    mode.placement = perf_cell(1u + search + i, 0u);
    mode.placement.align = VKR_UI_ALIGN_CENTER;
    vkr_editor_ghost_style(&mode);
    mode.style.font_size_pt = theme->font_caption;
    mode.style.padding_pt = (VkrUiEdges){2.0f, 8.0f, 2.0f, 8.0f};
    const bool8_t series = i == 1u;
    vkr_editor_toggle_style(&mode, view->show_series == series);
    mode.tooltip = string8_create_from_cstr((const uint8_t *)tips[i],
                                            string_length(tips[i]));
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("mode"),
                      string8_create_from_cstr((const uint8_t *)labels[i],
                                               string_length(labels[i])),
                      &mode)) {
      view->show_series = series;
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

/* The passes, most total time first unless a column sorts them; a click
   isolates one in the pass chart. */
vkr_internal void perf_pass_table(VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame,
                                  uint32_t column) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrRecordingData *data = &perf->data;
  const VkrRecordingAnalysis *analysis = &perf->analysis;
  if (!perf->has_passes) {
    perf_list_title(editor, ui, string8_lit("Passes: not recorded"), column);
    return;
  }
  perf_list_title(
      editor, ui,
      string8_create_formatted(
          ui->frame_allocator,
          "Passes: %u  \xc2\xb7  %s ms when they ran, over %llu "
          "frames; click one to isolate it",
          analysis->pass_count, perf->pass_gpu ? "GPU" : "CPU encode",
          (unsigned long long)(perf->pass_gpu ? data->pass_gpu_frames
                                              : data->pass_frames)),
      column);

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
  float64_t all_ms = 0.0;
  for (uint32_t i = 0u; i < analysis->pass_count; ++i) {
    all_ms += perf_pass_sum(perf, &analysis->passes[i]);
  }

  char at_title[32] = "At frame";
  if (at != UINT64_MAX) {
    string_format(at_title, sizeof(at_title), "At %llu",
                  (unsigned long long)at);
  }
  const VkrEditorTableColumn columns[PERF_PASS_COLUMN_COUNT] = {
      {.title = "Pass", .min_pt = 90.0f},
      {.title = "Share",
       .width_pt = 64.0f,
       .numeric = true_v,
       .tooltip = "The pass's part of all pass time over the recording"},
      {.title = "Mean",
       .width_pt = 66.0f,
       .numeric = true_v,
       .tooltip = "Mean ms in the frames the pass ran"},
      {.title = "p95", .width_pt = 62.0f, .numeric = true_v},
      {.title = "Max", .width_pt = 62.0f, .numeric = true_v},
      {.title = "CPU",
       .width_pt = 62.0f,
       .numeric = true_v,
       .tooltip = "Mean CPU encode ms"},
      {.title = at_title,
       .width_pt = 74.0f,
       .numeric = true_v,
       .tooltip = "Ms in the frame at the playhead, or the selected one"},
  };
  VkrEditorTable *table = &perf->view.passes;
  const uint32_t count = Min(analysis->pass_count, VKR_RECORDING_PASS_MAX);
  PerfSortItem items[VKR_RECORDING_PASS_MAX];
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrRecordingPassStats *stats = &analysis->passes[i];
    items[i] = (PerfSortItem){
        .key = perf_pass_key(perf, stats, table->sort_column, all_ms,
                             at_valid[stats->pass], at_ms[stats->pass]),
        .name = table->sort_column == PERF_PASS_COLUMN_NAME
                    ? data->passes[stats->pass].name
                    : NULL,
        .index = i};
  }
  perf_sort_items(table, items, count);
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, "passes", table, columns,
                              PERF_PASS_COLUMN_COUNT, count,
                              perf_cell(column, 1u), &sorted, &first, &end)) {
    return;
  }
  for (uint32_t r = first; r < end; ++r) {
    const VkrRecordingPassStats *stats = &analysis->passes[items[r].index];
    String8 cells[PERF_PASS_COLUMN_COUNT];
    perf_pass_cells(ui, perf, stats, all_ms, at_valid[stats->pass],
                    at_ms[stats->pass], cells);
    table->selected_key =
        perf->pass_focus == UINT32_MAX ? UINT64_MAX : perf->pass_focus;
    const VkrEditorTableRow row = {
        .key = stats->pass,
        .cells = cells,
        .dot = perf_pass_color(perf, stats->pass),
        .has_dot = true_v,
        .tooltip = string8_create_formatted(
            ui->frame_allocator, "%s  \xc2\xb7  click to show %s",
            data->passes[stats->pass].name,
            perf->pass_focus == stats->pass ? "every pass in the chart"
                                            : "only this pass in the chart"),
    };
    if (vkr_editor_table_row(ui, table, columns, PERF_PASS_COLUMN_COUNT, r,
                             &row)) {
      perf->pass_focus =
          perf->pass_focus == stats->pass ? UINT32_MAX : stats->pass;
      perf_pass_columns(perf);
    }
  }
  vkr_editor_table_end(ui);
}

/* Series table columns: name, unit, mean, p95 and max of the valid
   samples, and the frames that carry one. */
enum {
  PERF_SERIES_COLUMN_NAME = 0,
  PERF_SERIES_COLUMN_UNIT,
  PERF_SERIES_COLUMN_MEAN,
  PERF_SERIES_COLUMN_P95,
  PERF_SERIES_COLUMN_MAX,
  PERF_SERIES_COLUMN_FRAMES,
  PERF_SERIES_COLUMN_COUNT,
};

/* A value in its series' unit: bytes in binary multiples, whole counts
   without decimals. */
vkr_internal String8 perf_series_value(VkrAllocator *allocator,
                                       const char *unit, float64_t value) {
  const float64_t magnitude = vkr_abs_f64(value);
  if (string_equals(unit, "bytes")) {
    if (magnitude >= 1073741824.0) {
      return string8_create_formatted(allocator, "%.2f GiB",
                                      value / 1073741824.0);
    }
    if (magnitude >= 1048576.0) {
      return string8_create_formatted(allocator, "%.1f MiB", value / 1048576.0);
    }
    if (magnitude >= 1024.0) {
      return string8_create_formatted(allocator, "%.1f KiB", value / 1024.0);
    }
    return string8_create_formatted(allocator, "%.0f B", value);
  }
  if (!string_equals(unit, "ms") && magnitude < 1.0e15 &&
      vkr_floor_f64(value) == value) {
    return string8_create_formatted(allocator, "%.0f", value);
  }
  return string8_create_formatted(allocator, "%.3f", value);
}

/* Whether `name` holds the `length` bytes of `query`, ASCII
   case-insensitive; an empty query matches every name. */
vkr_internal bool8_t perf_series_matches(const char *name, const uint8_t *query,
                                         uint32_t length) {
  if (length == 0u) {
    return true_v;
  }
  const uint64_t name_length = string_length(name);
  for (uint64_t start = 0u; start + length <= name_length; ++start) {
    if (string_n_equalsi(name + start, (const char *)query, length)) {
      return true_v;
    }
  }
  return false_v;
}

/* The value series `index` sorts by in `column`. */
vkr_internal float64_t perf_series_key(const VkrRecordingStats *stats,
                                       uint32_t column, uint32_t index) {
  switch (column) {
  case PERF_SERIES_COLUMN_MEAN:
    return stats->mean;
  case PERF_SERIES_COLUMN_P95:
    return stats->p95;
  case PERF_SERIES_COLUMN_MAX:
    return stats->max;
  case PERF_SERIES_COLUMN_FRAMES:
    return (float64_t)stats->valid;
  default:
    return (float64_t)index;
  }
}

/* Every metrics series with a sample, in recording order unless a column
   sorts them: the Tier 0 fields, then an all-measurements recording's
   catalog metrics. */
vkr_internal void perf_series_table(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame,
                                    uint32_t column) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  VkrAllocator *allocator = ui->frame_allocator;
  const VkrRecordingData *data = &perf->data;
  const VkrRecordingAnalysis *analysis = &perf->analysis;
  VkrEditorTable *table = &perf->view.series;
  PerfSortItem *items =
      data->series_count > 0u
          ? vkr_allocator_alloc(allocator,
                                sizeof(*items) * (uint64_t)data->series_count,
                                VKR_ALLOCATOR_MEMORY_TAG_ARRAY)
          : NULL;
  const VkrEditorRecordingPerfView *view = &perf->view;
  uint32_t sampled = 0u;
  uint32_t count = 0u;
  for (uint32_t i = 0u; items && i < data->series_count; ++i) {
    const VkrRecordingStats *stats = &analysis->series[i];
    if (stats->valid == 0u) {
      continue;
    }
    ++sampled;
    if (!perf_series_matches(data->series[i].name, view->series_search,
                             view->series_search_length)) {
      continue;
    }
    const char *name = NULL;
    if (table->sort_column == PERF_SERIES_COLUMN_NAME) {
      name = data->series[i].name;
    } else if (table->sort_column == PERF_SERIES_COLUMN_UNIT) {
      name = data->series[i].unit;
    }
    items[count++] =
        (PerfSortItem){.key = perf_series_key(stats, table->sort_column, i),
                       .name = name,
                       .index = i};
  }
  String8 title = string8_lit("Series: none recorded");
  if (count < sampled) {
    title =
        string8_create_formatted(allocator, "Series: %u of %u", count, sampled);
  } else if (sampled > 0u) {
    title = string8_create_formatted(
        allocator, "Series: %u  \xc2\xb7  over the frames with a sample",
        sampled);
  }
  perf_list_title(editor, ui, title, column);
  if (count == 0u) {
    return;
  }
  static const VkrEditorTableColumn columns[PERF_SERIES_COLUMN_COUNT] = {
      {.title = "Series", .min_pt = 120.0f},
      {.title = "Unit", .width_pt = 56.0f},
      {.title = "Mean", .width_pt = 78.0f, .numeric = true_v},
      {.title = "p95", .width_pt = 78.0f, .numeric = true_v},
      {.title = "Max", .width_pt = 78.0f, .numeric = true_v},
      {.title = "Frames",
       .width_pt = 62.0f,
       .numeric = true_v,
       .tooltip = "Frames with a sample"},
  };
  perf_sort_items(table, items, count);
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, "series", table, columns,
                              PERF_SERIES_COLUMN_COUNT, count,
                              perf_cell(column, 1u), &sorted, &first, &end)) {
    return;
  }
  const String8 dash = string8_lit("\xe2\x80\x94");
  for (uint32_t r = first; r < end; ++r) {
    const uint32_t index = items[r].index;
    const VkrRecordingSeries *series = &data->series[index];
    const VkrRecordingStats *stats = &analysis->series[index];
    const String8 cells[PERF_SERIES_COLUMN_COUNT] = {
        string8_create_formatted(allocator, "%s", series->name),
        series->unit[0]
            ? string8_create_formatted(allocator, "%s", series->unit)
            : dash,
        perf_series_value(allocator, series->unit, stats->mean),
        perf_series_value(allocator, series->unit, stats->p95),
        perf_series_value(allocator, series->unit, stats->max),
        string8_create_formatted(allocator, "%llu",
                                 (unsigned long long)stats->valid),
    };
    const String8 low = perf_series_value(allocator, series->unit, stats->min);
    const String8 median =
        perf_series_value(allocator, series->unit, stats->p50);
    const String8 high = perf_series_value(allocator, series->unit, stats->p99);
    const VkrEditorTableRow row = {
        .key = index,
        .cells = cells,
        .tooltip = string8_create_formatted(
            allocator, "%s  \xc2\xb7  min %.*s, median %.*s, p99 %.*s",
            series->name, (int)low.length, (const char *)low.str,
            (int)median.length, (const char *)median.str, (int)high.length,
            (const char *)high.str),
    };
    (void)vkr_editor_table_row(ui, table, columns, PERF_SERIES_COLUMN_COUNT, r,
                               &row);
  }
  vkr_editor_table_end(ui);
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
                                            "%s (GiB)  \xc2\xb7  %.2f to %.2f",
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

/* What explains a spike: the GPU frame the frame loop waited on or the
   series that grew most, the pass whose GPU time grew most, and a marker
   within 10 frames. */
vkr_internal String8 perf_spike_cause(VkrUiSystem *ui,
                                      const VkrEditorRecordingPerf *perf,
                                      const VkrRecordingSpike *spike) {
  char cause[96] = "";
  if (spike->gpu_frame != UINT64_MAX) {
    /* The wait names the GPU frame it waited on. */
    string_format(cause, sizeof(cause), "GPU frame %llu +%.2f ms",
                  (unsigned long long)spike->gpu_frame, spike->gpu_excess);
  } else if (spike->contributor_count > 0u) {
    const VkrRecordingContributor *top = &spike->contributors[0];
    string_format(cause, sizeof(cause), "%s +%.2f ms",
                  perf->data.series[top->series].name, top->excess);
  }
  /* CPU encode times barely move at a spike; only GPU pass times explain
     one here. */
  char pass[112] = "";
  if (spike->pass_gpu && spike->pass_contributor_count > 0u) {
    const VkrRecordingPassContributor *top = &spike->passes[0];
    string_format(pass, sizeof(pass), "%s%s GPU +%.2f ms",
                  cause[0] ? "  \xc2\xb7  " : "",
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
  return string8_create_formatted(ui->frame_allocator, "%s%s%s%s", cause, pass,
                                  marker[0] ? "  \xc2\xb7  near " : "", marker);
}

enum {
  PERF_SPIKE_COLUMN_RANK = 0,
  PERF_SPIKE_COLUMN_FRAME,
  PERF_SPIKE_COLUMN_TIME,
  PERF_SPIKE_COLUMN_MS,
  PERF_SPIKE_COLUMN_RATIO,
  PERF_SPIKE_COLUMN_CAUSE,
  PERF_SPIKE_COLUMN_COUNT,
};

vkr_internal float64_t perf_spike_key(const VkrRecordingSpike *spike,
                                      uint32_t rank, uint32_t column) {
  switch (column) {
  case PERF_SPIKE_COLUMN_FRAME:
    return (float64_t)spike->frame;
  case PERF_SPIKE_COLUMN_TIME:
    return spike->time;
  case PERF_SPIKE_COLUMN_MS:
    return spike->frame_ms;
  case PERF_SPIKE_COLUMN_RATIO:
    return spike->ratio;
  default:
    return (float64_t)rank;
  }
}

/* The spikes, worst first unless a column sorts them; a click watches the
   frame in the Scene. */
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
  const uint32_t count =
      Min(analysis->spike_count, (uint32_t)VKR_RECORDING_ANALYSIS_SPIKE_MAX);
  if (count == 0u) {
    perf_caption(ui, "spikes.none",
                 string8_lit("No spikes: every frame stays near its "
                             "neighbours."),
                 perf_cell(column, 1u), theme->text_secondary);
    return;
  }
  static const VkrEditorTableColumn columns[PERF_SPIKE_COLUMN_COUNT] = {
      {.title = "#", .width_pt = 34.0f, .numeric = true_v},
      {.title = "Frame", .width_pt = 64.0f, .numeric = true_v},
      {.title = "Time s", .width_pt = 64.0f, .numeric = true_v},
      {.title = "ms", .width_pt = 58.0f, .numeric = true_v},
      {.title = "Ratio",
       .width_pt = 60.0f,
       .numeric = true_v,
       .tooltip = "Frame time over the median of its 19 neighbours"},
      {.title = "Cause",
       .min_pt = 90.0f,
       .tooltip = "The series and the GPU pass that grew most, and a marker "
                  "within 10 frames"},
  };
  VkrEditorTable *table = &perf->view.spikes;
  PerfSortItem items[VKR_RECORDING_ANALYSIS_SPIKE_MAX];
  for (uint32_t i = 0u; i < count; ++i) {
    items[i] = (PerfSortItem){
        .key = perf_spike_key(&analysis->spikes[i], i, table->sort_column),
        .index = i};
  }
  perf_sort_items(table, items, count);
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, "spikes", table, columns,
                              PERF_SPIKE_COLUMN_COUNT, count,
                              perf_cell(column, 1u), &sorted, &first, &end)) {
    return;
  }
  table->selected_key = perf->selected;
  for (uint32_t r = first; r < end; ++r) {
    const uint32_t i = items[r].index;
    const VkrRecordingSpike *spike = &analysis->spikes[i];
    VkrAllocator *allocator = ui->frame_allocator;
    const String8 cells[PERF_SPIKE_COLUMN_COUNT] = {
        string8_create_formatted(allocator, "%u", i + 1u),
        string8_create_formatted(allocator, "%llu",
                                 (unsigned long long)spike->frame),
        string8_create_formatted(allocator, "%.2f", spike->time),
        string8_create_formatted(allocator, "%.2f", spike->frame_ms),
        string8_create_formatted(allocator, "%.1fx", spike->ratio),
        perf_spike_cause(ui, perf, spike),
    };
    const VkrEditorTableRow row = {
        .key = spike->frame,
        .cells = cells,
        .text_color = spike->frame_ms > 2.0 * analysis->budget_ms ? theme->error
                                                                  : theme->text,
        /* The cause column cuts a long cause; the tooltip keeps it. */
        .tooltip = string8_create_formatted(
            allocator, "%.*s\nClick to watch this frame in the Scene",
            (int)cells[PERF_SPIKE_COLUMN_CAUSE].length,
            (const char *)cells[PERF_SPIKE_COLUMN_CAUSE].str),
    };
    if (vkr_editor_table_row(ui, table, columns, PERF_SPIKE_COLUMN_COUNT, r,
                             &row)) {
      perf_seek(editor, frame, spike->frame);
    }
  }
  vkr_editor_table_end(ui);
}

/* Nothing analyzed yet: what the window shows and a way to fill it, in the
   middle of the window. */
vkr_internal void perf_empty(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrEditorRecordingEntry *newest = vkr_editor_recording_newest(editor);
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack rows[] = {auto_track, auto_track, auto_track, auto_track};
  VkrUiPanelConfig body = perf_grid(perf_cell(0u, 0u), &fill, 1u, &fill, 1u);
  if (!vkr_ui_panel_begin(ui, string8_lit("perf.empty"), &body)) {
    return;
  }
  VkrUiPanelConfig stack =
      perf_grid(perf_cell(0u, 0u), &auto_track, 1u, rows, ArrayCount(rows));
  stack.placement.justify = stack.placement.align = VKR_UI_ALIGN_CENTER;
  stack.style.gap_pt = theme->space_md;
  if (vkr_ui_panel_begin(ui, string8_lit("stack"), &stack)) {
    VkrUiWidgetConfig icon = vkr_ui_widget_config_default();
    icon.placement = perf_cell(0u, 0u);
    icon.placement.justify = VKR_UI_ALIGN_CENTER;
    icon.icon = VKR_UI_ICON_CHART_BAR;
    icon.icon_size_pt = 32.0f;
    icon.icon_color = theme->text_disabled;
    vkr_ui_label(ui, string8_lit("icon"), (String8){0}, &icon);
    VkrUiWidgetConfig title =
        vkr_editor_text_config(theme->font_body, theme->text);
    title.placement = perf_cell(0u, 1u);
    title.placement.justify = VKR_UI_ALIGN_CENTER;
    title.text.font = editor->heading_font;
    vkr_ui_label(ui, string8_lit("title"), string8_lit("No recording analyzed"),
                 &title);
    VkrUiWidgetConfig text =
        vkr_editor_text_config(theme->font_body, theme->text_secondary);
    text.placement = perf_cell(0u, 2u);
    text.placement.justify = VKR_UI_ALIGN_CENTER;
    text.text.layout.word_wrap = true_v;
    text.text.layout.max_width = 360.0f;
    text.text.layout.anchor.horizontal = VKR_TEXT_ALIGN_CENTER;
    vkr_ui_label(ui, string8_lit("text"),
                 string8_lit("Frame times, spikes, CPU stages and GPU passes "
                             "of a recording show here. Analyze one from its "
                             "card in Recordings or from the playback bar."),
                 &text);
    VkrUiWidgetConfig analyze = vkr_ui_widget_config_default();
    analyze.placement = perf_cell(0u, 3u);
    analyze.placement.justify = VKR_UI_ALIGN_CENTER;
    analyze.style.margin_pt.top = theme->space_sm;
    vkr_editor_primary_style(&analyze, editor->heading_font);
    analyze.icon = VKR_UI_ICON_CHART_BAR;
    vkr_editor_button_size(&analyze);
    analyze.disabled = newest == NULL;
    analyze.tooltip = newest ? string8_lit("Analyze the newest recording")
                             : string8_lit("Record something first");
    if (vkr_ui_button(ui, string8_lit("analyze"),
                      newest
                          ? string8_create_formatted(ui->frame_allocator,
                                                     "Analyze %s", newest->name)
                          : string8_lit("No recordings yet"),
                      &analyze) &&
        newest) {
      char error[160];
      char path[VKR_RECORDING_PATH_MAX];
      string_format(path, sizeof(path), "%s/%s", editor->recording.directory,
                    newest->bundle);
      if (!vkr_editor_recording_analyze(editor, path, error, sizeof(error))) {
        vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, theme->warning,
                         error);
      }
    }
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

/* ------------------------------------------------------------------------ */
/* Compare                                                                   */

/* The header's comparison controls: what B is, how it is aligned, and an
   end to it. */
vkr_internal void perf_compare_controls(VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame,
                                        uint32_t column) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack columns[] = {auto_track, auto_track, auto_track,
                                auto_track, auto_track, auto_track};
  VkrUiPanelConfig row =
      perf_grid(perf_cell(column, 0u), columns, ArrayCount(columns), &fill, 1u);
  row.placement.justify = VKR_UI_ALIGN_END;
  row.style.gap_pt = theme->space_xs;
  if (!vkr_ui_panel_begin(ui, string8_lit("compare"), &row)) {
    return;
  }
  VkrUiWidgetConfig with =
      vkr_editor_text_config(theme->font_caption, theme->text);
  with.placement = perf_cell(0u, 0u);
  with.placement.align = VKR_UI_ALIGN_CENTER;
  with.style.padding_pt.right = theme->space_md;
  with.icon = VKR_UI_ICON_DOT;
  with.icon_size_pt = 10.0f;
  with.icon_color = PERF_COMPARE_COLOR;
  with.tooltip = string8_create_formatted(ui->frame_allocator,
                                          "B: %s, compared with A: %s",
                                          perf->compare_path, perf->path);
  vkr_ui_label(ui, string8_lit("with"),
               string8_create_formatted(ui->frame_allocator, "vs %s",
                                        perf->compare_name),
               &with);
  static const char *const labels[] = {"Time", "Marker", "Distance", "Frame"};
  static const char *const tips[] = {
      "Align by seconds from the first frame",
      "Align on a marker label both recordings carry",
      "Align by meters along the path: two runs at different speeds line up",
      "Align frame by frame: re-runs of one recording only",
  };
  _Static_assert(ArrayCount(labels) == VKR_RECORDING_ALIGN_COUNT,
                 "a label for every alignment");
  for (uint32_t i = 0u; i < VKR_RECORDING_ALIGN_COUNT; ++i) {
    VkrUiWidgetConfig mode = vkr_ui_widget_config_default();
    mode.placement = perf_cell(1u + i, 0u);
    mode.placement.align = VKR_UI_ALIGN_CENTER;
    vkr_editor_ghost_style(&mode);
    mode.style.font_size_pt = theme->font_caption;
    mode.style.padding_pt = (VkrUiEdges){2.0f, 8.0f, 2.0f, 8.0f};
    vkr_editor_toggle_style(&mode, perf->compare.align == i);
    mode.tooltip =
        i == VKR_RECORDING_ALIGN_MARKER && perf->compare.marker[0]
            ? string8_create_formatted(ui->frame_allocator, "%s: \"%s\"",
                                       tips[i], perf->compare.marker)
            : string8_create_from_cstr((const uint8_t *)tips[i],
                                       string_length(tips[i]));
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("align"),
                      string8_create_from_cstr((const uint8_t *)labels[i],
                                               string_length(labels[i])),
                      &mode) &&
        perf->compare.align != i) {
      char error[160];
      char path[VKR_RECORDING_PATH_MAX];
      string_format(path, sizeof(path), "%s", perf->compare_path);
      if (!vkr_editor_recording_compare(editor, path, (VkrRecordingAlign)i,
                                        NULL, error, sizeof(error))) {
        vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, theme->warning,
                         error);
      }
    }
    (void)vkr_ui_pop_id(ui);
  }
  VkrUiWidgetConfig stop = vkr_editor_icon_button_config(
      5u, 0u, VKR_UI_ICON_CLOSE, string8_lit("Stop comparing"));
  if (vkr_ui_button(ui, string8_lit("stop"), (String8){0}, &stop)) {
    vkr_editor_recording_compare_stop(perf);
    perf_columns(perf);
  }
  (void)vkr_ui_panel_end(ui);
}

/* The window's first row: what A is, and the comparison when one is open. */
vkr_internal void perf_header(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame, uint32_t row) {
  const VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrRecordingData *data = &perf->data;
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack columns[] = {fill, {.unit = VKR_UI_TRACK_AUTO}};
  VkrUiPanelConfig header = perf_grid(perf_cell(0u, row), columns,
                                      perf->comparing ? 2u : 1u, &fill, 1u);
  if (!vkr_ui_panel_begin(ui, string8_lit("header"), &header)) {
    return;
  }
  /* Where B is slower, when it is anywhere; else how much faster. */
  char window[112] = "";
  const VkrRecordingCompare *compare = &perf->compare;
  if (perf->comparing && compare->worst_first != UINT64_MAX &&
      compare->worst_delta_ms > 0.0) {
    string_format(window, sizeof(window),
                  "  \xc2\xb7  B slower at frames %llu to %llu by %.2f ms",
                  (unsigned long long)compare->worst_first,
                  (unsigned long long)compare->worst_last,
                  compare->worst_delta_ms);
  } else if (perf->comparing && compare->aligned_frames > 0u) {
    string_format(window, sizeof(window),
                  "  \xc2\xb7  B %+.2f ms per aligned frame",
                  compare->delta_mean_ms);
  }
  perf_title(ui, "title",
             string8_create_formatted(
                 ui->frame_allocator,
                 "%s%s  \xc2\xb7  %.1f s  \xc2\xb7  %llu frames%s%s%s",
                 perf->comparing ? "A: " : "", perf->name,
                 data->times[data->frame_count - 1u],
                 (unsigned long long)data->frame_count,
                 data->missing_frames ? "  \xc2\xb7  frames lost" : "",
                 data->truncated ? "  \xc2\xb7  cut off" : "", window),
             perf_cell(0u, 0u), false_v);
  if (perf->comparing) {
    perf_compare_controls(editor, frame, 1u);
  }
  (void)vkr_ui_panel_end(ui);
}

/* The pass table while comparing: A's and B's mean per pass by name, the
   change and its percentage. */
enum {
  PERF_DELTA_COLUMN_NAME = 0,
  PERF_DELTA_COLUMN_A,
  PERF_DELTA_COLUMN_B,
  PERF_DELTA_COLUMN_CHANGE,
  PERF_DELTA_COLUMN_PERCENT,
  PERF_DELTA_COLUMN_COUNT,
};

vkr_internal float64_t perf_delta_key(const VkrRecordingPassDelta *delta,
                                      uint32_t column, uint32_t rank) {
  switch (column) {
  case PERF_DELTA_COLUMN_A:
    return delta->in_a ? delta->a_ms : -1.0;
  case PERF_DELTA_COLUMN_B:
    return delta->in_b ? delta->b_ms : -1.0;
  case PERF_DELTA_COLUMN_CHANGE:
    return delta->b_ms - delta->a_ms;
  case PERF_DELTA_COLUMN_PERCENT:
    return delta->in_a && delta->in_b && delta->a_ms > 0.0
               ? (delta->b_ms - delta->a_ms) / delta->a_ms
               : -1.0e9;
  default:
    return (float64_t)rank;
  }
}

/* The passes of both recordings, largest change first unless a column
   sorts them. */
vkr_internal void perf_compare_pass_table(VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame,
                                          uint32_t column) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrRecordingCompare *compare = &perf->compare;
  VkrUiSystem *ui = frame->ui;
  if (compare->pass_count == 0u) {
    perf_title(ui, "passes.title",
               string8_lit("Passes: neither recording has pass rows"),
               perf_cell(column, 0u), false_v);
    return;
  }
  perf_title(ui, "passes.title",
             string8_create_formatted(ui->frame_allocator,
                                      "Passes: mean %s ms per frame, B minus A",
                                      compare->pass_gpu ? "GPU" : "CPU encode"),
             perf_cell(column, 0u), false_v);
  static const VkrEditorTableColumn columns[PERF_DELTA_COLUMN_COUNT] = {
      {.title = "Pass", .min_pt = 90.0f},
      {.title = "A", .width_pt = 64.0f, .numeric = true_v},
      {.title = "B", .width_pt = 64.0f, .numeric = true_v},
      {.title = "Change", .width_pt = 70.0f, .numeric = true_v},
      {.title = "%", .width_pt = 56.0f, .numeric = true_v},
  };
  VkrEditorTable *table = &perf->view.passes_compare;
  const uint32_t count = Min(compare->pass_count, VKR_RECORDING_PASS_MAX);
  PerfSortItem items[VKR_RECORDING_PASS_MAX];
  for (uint32_t i = 0u; i < count; ++i) {
    items[i] = (PerfSortItem){
        .key = perf_delta_key(&compare->passes[i], table->sort_column, i),
        .name = table->sort_column == PERF_DELTA_COLUMN_NAME
                    ? compare->passes[i].name
                    : NULL,
        .index = i};
  }
  perf_sort_items(table, items, count);
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, "compare.passes", table, columns,
                              PERF_DELTA_COLUMN_COUNT, count,
                              perf_cell(column, 1u), &sorted, &first, &end)) {
    return;
  }
  const String8 dash = string8_lit("\xe2\x80\x94");
  for (uint32_t r = first; r < end; ++r) {
    const VkrRecordingPassDelta *delta = &compare->passes[items[r].index];
    VkrAllocator *allocator = ui->frame_allocator;
    const bool8_t both = delta->in_a && delta->in_b && delta->a_ms > 0.0;
    const String8 cells[PERF_DELTA_COLUMN_COUNT] = {
        string8_create_formatted(allocator, "%s", delta->name),
        delta->in_a ? string8_create_formatted(allocator, "%.3f", delta->a_ms)
                    : dash,
        delta->in_b ? string8_create_formatted(allocator, "%.3f", delta->b_ms)
                    : dash,
        string8_create_formatted(allocator, "%+.3f", delta->b_ms - delta->a_ms),
        both ? string8_create_formatted(allocator, "%+.0f%%",
                                        100.0 * (delta->b_ms - delta->a_ms) /
                                            delta->a_ms)
             : dash,
    };
    const VkrEditorTableRow row = {
        .key = items[r].index,
        .cells = cells,
        .text_color = perf_compare_color(delta->a_ms, delta->b_ms),
        .tooltip = string8_create_formatted(allocator, "%s%s", delta->name,
                                            !delta->in_a   ? ": only in B"
                                            : !delta->in_b ? ": only in A"
                                                           : ""),
    };
    (void)vkr_editor_table_row(ui, table, columns, PERF_DELTA_COLUMN_COUNT, r,
                               &row);
  }
  vkr_editor_table_end(ui);
}

void vkr_editor_recording_perf_build(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     VkrUiRect bounds) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  if (!perf->loaded) {
    perf_empty(editor, frame);
    return;
  }
  /* The charts over the lists, and the spikes beside the passes, each
     pair split by a bar the designer drags. */
  VkrEditorRecordingPerfView *view = &perf->view;
  if (view->charts.ratio <= 0.0f) {
    view->charts =
        (VkrEditorSplit){.ratio = 0.68f, .min_ratio = 0.3f, .max_ratio = 0.88f};
    view->lists =
        (VkrEditorSplit){.ratio = 0.5f, .min_ratio = 0.2f, .max_ratio = 0.8f};
  }
  const float32_t cards_pt = perf->comparing ? 66.0f : 50.0f;
  const float32_t scale = ui->content_scale;
  const float32_t gap = theme->space_sm;
  const VkrUiTrack title = {.value = 18.0f, .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack bar = {.value = 6.0f, .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack rows[] = {
      title,
      {.value = cards_pt, .unit = VKR_UI_TRACK_PX},
      {.value = view->charts.ratio, .unit = VKR_UI_TRACK_FR},
      bar,
      {.value = 1.0f - view->charts.ratio, .unit = VKR_UI_TRACK_FR},
  };
  VkrUiPanelConfig body =
      perf_grid(perf_cell(0u, 0u), &fill, 1u, rows, ArrayCount(rows));
  body.style.padding_pt = (VkrUiEdges){10.0f, 12.0f, 10.0f, 12.0f};
  body.style.gap_pt = gap;
  body.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("perf.body"), &body)) {
    return;
  }
  perf_header(editor, frame, 0u);
  perf_cards(editor, ui, 1u);
  const VkrUiTrack chart_rows[] = {
      title,
      {.value = 2.0f, .unit = VKR_UI_TRACK_FR},
      title,
      {.value = 1.2f, .unit = VKR_UI_TRACK_FR},
      title,
      {.value = perf->has_passes ? 1.4f : 0.0f, .unit = VKR_UI_TRACK_FR},
      {.value = 84.0f, .unit = VKR_UI_TRACK_PX},
  };
  VkrUiPanelConfig charts = perf_grid(perf_cell(0u, 2u), &fill, 1u, chart_rows,
                                      ArrayCount(chart_rows));
  charts.style.gap_pt = gap;
  if (vkr_ui_panel_begin(ui, string8_lit("perf.charts"), &charts)) {
    perf_frame_chart(editor, frame, 0u);
    perf_stage_chart(editor, frame, 2u);
    perf_pass_chart(editor, frame, 4u);
    perf_memory_charts(editor, frame, 6u);
    (void)vkr_ui_panel_end(ui);
  }
  /* The panes' joint lengths: the window's height under the cards, and
     its width, without padding, gaps and bars. */
  const float32_t split_height =
      bounds.height / scale - 20.0f - 18.0f - cards_pt - 6.0f - 4.0f * gap;
  const float32_t split_width =
      bounds.width / scale - 24.0f - 6.0f - 2.0f * theme->space_md;
  vkr_editor_split_bar(ui, "perf.split.charts", &view->charts,
                       perf_cell(0u, 3u), true_v, split_height);
  /* Spikes and passes side by side, each a title over a table. */
  const VkrUiTrack columns[] = {
      {.value = view->lists.ratio, .unit = VKR_UI_TRACK_FR},
      bar,
      {.value = 1.0f - view->lists.ratio, .unit = VKR_UI_TRACK_FR}};
  /* The list titles hold the Passes/Series switch and the series search,
     so they take a control's height. */
  const VkrUiTrack list_title = {.value = theme->control_height,
                                 .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack lists[] = {list_title, fill};
  VkrUiPanelConfig bottom =
      perf_grid(perf_cell(0u, 4u), columns, ArrayCount(columns), lists,
                ArrayCount(lists));
  bottom.style.gap_pt = theme->space_md;
  if (vkr_ui_panel_begin(ui, string8_lit("perf.lists"), &bottom)) {
    perf_spikes(editor, frame, 0u);
    VkrUiPlacement split = perf_cell(1u, 0u);
    split.row_span = 2u;
    vkr_editor_split_bar(ui, "perf.split.lists", &view->lists, split, false_v,
                         split_width);
    if (perf->comparing) {
      perf_compare_pass_table(editor, frame, 2u);
    } else if (view->show_series) {
      perf_series_table(editor, frame, 2u);
    } else {
      perf_pass_table(editor, frame, 2u);
    }
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_panel_end(ui);
}
