#include "editor_recording.h"

#include "containers/vkr_sort.h"
#include "core/vkr_atomic.h"
#include "core/vkr_content_codec.h"
#include "core/vkr_job_system.h"
#include "editor_internal.h"
#include "editor_ui.h"
#include "memory/vkr_dmemory_allocator.h"
#include "memory/vkr_heap.h"
#include "platform/vkr_platform.h"

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
/* Heights of the tab row and of the frame strip over the tabs but
   Overview. */
#define PERF_TAB_ROW_PT 30.0f
#define PERF_STRIP_PT 44.0f
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

typedef struct VkrEditorPerfLoad VkrEditorPerfLoad;
vkr_internal void perf_load_drop(VkrEditorPerfLoad *load);

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
    vkr_editor_recording_perf_draws_release(perf);
    vkr_recording_analysis_free(&perf->analysis);
    vkr_recording_data_free(&perf->data);
    perf_load_drop(perf->source);
  }
  perf_load_drop(perf->loading);
  perf->loading = NULL;
  for (uint32_t i = 0u; i < perf->retired_count; ++i) {
    perf_load_drop(perf->retired[i]);
  }
  perf->retired_count = 0u;
  perf->source = NULL;
  perf->allocator = NULL;
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

vkr_internal int32_t perf_compare_f32(const void *lhs, const void *rhs) {
  const float32_t a = *(const float32_t *)lhs;
  const float32_t b = *(const float32_t *)rhs;
  return a < b ? -1 : (a > b ? 1 : 0);
}

vkr_internal int32_t perf_compare_f64(const void *lhs, const void *rhs) {
  const float64_t a = *(const float64_t *)lhs;
  const float64_t b = *(const float64_t *)rhs;
  return a < b ? -1 : (a > b ? 1 : 0);
}

/* The shown frames' frame-time statistics for the cards: percentiles of the
   valid frame times, the time-based 1% low (the frame time the slowest 1% of
   the shown time reaches), and the frames over the budget. */
vkr_internal void perf_view_stats(VkrEditorRecordingPerf *perf) {
  const VkrRecordingData *data = &perf->data;
  const uint64_t count = perf->view_last + 1u - perf->view_first;
  perf->view_time = (VkrRecordingStats){0};
  perf->view_low_1pct = 0.0;
  perf->view_over_budget = 0u;
  float64_t *values =
      vkr_allocator_alloc(perf->allocator, sizeof(float64_t) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!values) {
    return;
  }
  uint64_t valid = 0u;
  float64_t sum = 0.0;
  for (uint64_t f = perf->view_first; f <= perf->view_last; ++f) {
    float64_t ms = 0.0;
    if (vkr_recording_data_frame_ms(data, f, &ms)) {
      values[valid++] = ms;
      sum += ms;
      perf->view_over_budget += ms > perf->analysis.budget_ms ? 1u : 0u;
    }
  }
  if (valid > 0u) {
    vkr_sort(values, valid, sizeof(float64_t), perf_compare_f64);
    const VkrRecordingStats stats = {
        .valid = valid,
        .min = values[0],
        .max = values[valid - 1u],
        .mean = sum / (float64_t)valid,
        .p50 = values[(valid - 1u) * 50u / 100u],
        .p95 = values[(valid - 1u) * 95u / 100u],
        .p99 = values[(valid - 1u) * 99u / 100u],
    };
    perf->view_time = stats;
    float64_t slow = 0.0;
    for (uint64_t i = valid; i-- > 0u;) {
      slow += values[i];
      if (slow >= 0.01 * sum) {
        perf->view_low_1pct = values[i];
        break;
      }
    }
  }
  vkr_allocator_free(perf->allocator, values, sizeof(float64_t) * count,
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
}

/* Reduces the shown frames to at most VKR_EDITOR_PERF_COLUMNS columns, each
   showing the slowest frame of its run so no spike disappears. */
vkr_internal void perf_columns(VkrEditorRecordingPerf *perf) {
  const VkrRecordingData *data = &perf->data;
  if (perf->view_last >= data->frame_count ||
      perf->view_first > perf->view_last) {
    perf->view_first = 0u;
    perf->view_last = data->frame_count - 1u;
  }
  const uint64_t base = perf->view_first;
  const uint64_t frames = perf->view_last + 1u - base;
  const uint32_t columns =
      (uint32_t)Min(frames, (uint64_t)VKR_EDITOR_PERF_COLUMNS);
  uint32_t stage[VKR_EDITOR_PERF_STAGES];
  perf->has_stages = false_v;
  for (uint32_t s = 0u; s < VKR_EDITOR_PERF_STAGES; ++s) {
    stage[s] = vkr_recording_data_find_series(data, PERF_STAGE_SERIES[s]);
    perf->has_stages = perf->has_stages || stage[s] != UINT32_MAX;
  }
  const uint32_t ram =
      vkr_recording_data_find_series(data, "process.resident_bytes");
  const uint32_t vram = vkr_recording_data_find_series(
      data, "memory.gpu.driver.current_allocated_bytes");
  const uint32_t charted =
      perf->chart_series < data->series_count ? perf->chart_series : UINT32_MAX;
  const float64_t budget = perf->analysis.budget_ms;
  perf->ram_range[0] = perf->vram_range[0] = perf->series_range[0] = 1.0e30f;
  perf->ram_range[1] = perf->vram_range[1] = perf->series_range[1] = -1.0e30f;
  perf->has_ram = perf->has_vram = false_v;
  float32_t ram_last = 0.0f;
  float32_t vram_last = 0.0f;
  float32_t series_last = 0.0f;
  float32_t stage_top = 0.0f;
  float32_t frame_max = 0.0f;
  float32_t stage_sums[VKR_EDITOR_PERF_COLUMNS];
  for (uint32_t c = 0u; c < columns; ++c) {
    const uint64_t first = base + (uint64_t)c * frames / columns;
    const uint64_t last =
        Max(first + 1u, base + (uint64_t)(c + 1u) * frames / columns);
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
    frame_max = Max(frame_max, perf->frame_ms[c]);
    perf->frame_color[c] = worst_ms < 0.0            ? PERF_FRAME_NONE
                           : worst_ms > 2.0 * budget ? PERF_FRAME_HITCH
                           : worst_ms > budget       ? PERF_FRAME_SLOW
                                                     : PERF_FRAME_OK;
    float32_t stage_sum = 0.0f;
    for (uint32_t s = 0u; s < VKR_EDITOR_PERF_STAGES; ++s) {
      float64_t value = 0.0;
      perf->stage_ms[s][c] = !perf->view.stage_hidden[s] &&
                                     perf_value(data, stage[s], worst, &value)
                                 ? (float32_t)value
                                 : 0.0f;
      stage_sum += perf->stage_ms[s][c];
    }
    stage_top = Max(stage_top, stage_sum);
    stage_sums[c] = stage_sum;
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
    /* The charted series' largest value over the column's frames. */
    if (charted != UINT32_MAX) {
      bool8_t any = false_v;
      float64_t high = 0.0;
      for (uint64_t f = first; f < last; ++f) {
        float64_t value = 0.0;
        if (perf_value(data, charted, f, &value) && (!any || value > high)) {
          high = value;
          any = true_v;
        }
      }
      if (any) {
        series_last = (float32_t)high;
        perf_range(perf->series_range, series_last);
      }
      perf->series_values[c] = series_last;
    }
    perf->ram_gb[c] = ram_last;
    perf->vram_gb[c] = vram_last;
  }
  perf->column_count = columns;
  ++perf->columns_version;
  /* One giant hitch must not flatten every other bar: the range stops at
     three times the 99th percentile; taller bars clip at the top. */
  perf_view_stats(perf);
  const VkrRecordingStats *time = &perf->view_time;
  perf->frame_ms_top =
      (float32_t)Max(budget * 1.5, Min((float64_t)frame_max * 1.05,
                                       Max(time->p99 * 3.0, budget * 2.2)));
  /* As for frame times, one stalled frame must not flatten the stages. */
  vkr_sort(stage_sums, columns, sizeof(float32_t), perf_compare_f32);
  const float32_t stage_p99 =
      columns > 0u ? stage_sums[(columns - 1u) * 99u / 100u] : 0.0f;
  perf->stage_ms_top =
      Max(Min(stage_top * 1.05f, Max(stage_p99 * 3.0f, 1.0f)), 1.0f);
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

  /* The shown frames that carry the chart's times. */
  const uint64_t view_last = Min(perf->view_last, data->frame_count - 1u);
  uint64_t eligible = 0u;
  for (uint64_t f = perf->view_first; f <= view_last; ++f) {
    eligible += perf_pass_frame(perf, f) ? 1u : 0u;
  }
  perf->pass_view_frames = eligible;
  const uint32_t columns =
      (uint32_t)Min(eligible, (uint64_t)VKR_EDITOR_PERF_COLUMNS);
  float32_t totals[VKR_EDITOR_PERF_COLUMNS];
  uint64_t k = 0u;
  for (uint64_t f = perf->view_first; f <= view_last && columns > 0u; ++f) {
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
  /* A is about to change: B would be compared with the wrong one. */
  if (perf->loading) {
    string_format(error, error_capacity,
                  "Wait for the recording being read; it becomes A.");
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
    if (vkr_recording_data_load(&data, perf->allocator, path,
                                vkr_content_codec_zstd_decoder()) !=
            VKR_RECORDING_DATA_OK ||
        data.frame_count == 0u) {
      string_format(error, error_capacity, "'%s' is not a readable recording.",
                    path);
      return false_v;
    }
    if (!vkr_recording_analyze(&data, NULL, perf->allocator, &analysis)) {
      vkr_recording_data_free(&data);
      string_format(error, error_capacity,
                    "The recording could not be analyzed.");
      return false_v;
    }
  }
  const VkrRecordingCompareOptions options = {.align = align, .marker = marker};
  VkrRecordingCompare compare;
  if (!vkr_recording_compare(&perf->data, &perf->analysis, &data, &analysis,
                             &options, perf->allocator, &compare, error,
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

/* A recording read and analyzed off the frame loop, in memory it owns; the
   Performance window adopts it whole and keeps it as `source`. Allocated
   from the process heap, because the job worker reads into it and the
   frame loop frees it. */
struct VkrEditorPerfLoad {
  /* The job system it was read on, to wait for before freeing it. */
  VkrJobSystem *jobs;
  VkrDMemory memory;
  VkrAllocator allocator;
  bool8_t memory_ready;
  char path[VKR_RECORDING_PATH_MAX];
  VkrRecordingData data;
  VkrRecordingAnalysis analysis;
  /* Set when the read is given up on, so the job skips what is left. */
  VkrAtomicUint32 cancel;
  /* Set by the job last; until then only the job touches the rest. */
  VkrAtomicUint32 done;
  bool8_t ok;
  char error[160];
  VkrJobHandle job;
  bool8_t queued;
  /* When the read was asked for, for the elapsed time it shows. */
  float64_t started;
};

/* Frees a load and the memory everything read lives in. */
vkr_internal void perf_load_free(VkrEditorPerfLoad *load) {
  if (!load) {
    return;
  }
  if (load->memory_ready) {
    vkr_dmemory_allocator_destroy(&load->allocator);
  }
  vkr_heap_free(load);
}

/* On a job worker: reads and analyzes the load's recording. */
vkr_internal bool8_t perf_load_job(VkrJobContext *context, void *payload) {
  (void)context;
  VkrEditorPerfLoad *load = *(VkrEditorPerfLoad **)payload;
  load->ok = false_v;
  if (!vkr_dmemory_create(PERF_MEMORY_COMMIT, PERF_MEMORY_RESERVE,
                          &load->memory)) {
    string_format(load->error, sizeof(load->error), "Out of memory.");
  } else {
    load->memory_ready = true_v;
    load->allocator = (VkrAllocator){.ctx = &load->memory};
    vkr_dmemory_allocator_create(&load->allocator);
    if (vkr_recording_data_load(&load->data, &load->allocator, load->path,
                                vkr_content_codec_zstd_decoder()) !=
            VKR_RECORDING_DATA_OK ||
        load->data.frame_count == 0u) {
      string_format(load->error, sizeof(load->error),
                    "'%s' is not a readable recording.", load->path);
    } else if (vkr_atomic_uint32_load(&load->cancel,
                                      VKR_MEMORY_ORDER_RELAXED)) {
      vkr_recording_data_free(&load->data);
      string_format(load->error, sizeof(load->error), "Cancelled.");
    } else if (!vkr_recording_analyze(&load->data, NULL, &load->allocator,
                                      &load->analysis)) {
      vkr_recording_data_free(&load->data);
      string_format(load->error, sizeof(load->error),
                    "The recording could not be analyzed.");
    } else {
      load->ok = true_v;
    }
  }
  vkr_atomic_uint32_store(&load->done, 1u, VKR_MEMORY_ORDER_RELEASE);
  return true_v;
}

/* Waits for a load's job, if it has one, then frees it. */
vkr_internal void perf_load_drop(VkrEditorPerfLoad *load) {
  if (!load) {
    return;
  }
  if (load->queued) {
    vkr_atomic_uint32_store(&load->cancel, 1u, VKR_MEMORY_ORDER_RELAXED);
    (void)vkr_job_wait(load->jobs, load->job);
    load->queued = false_v;
  }
  perf_load_free(load);
}

/* Gives up on a read without waiting for it: a finished one is freed now,
   a running one when the poll sees it done. */
vkr_internal void perf_load_abandon(VkrEditorRecordingPerf *perf,
                                    VkrEditorPerfLoad *load) {
  if (!load) {
    return;
  }
  vkr_atomic_uint32_store(&load->cancel, 1u, VKR_MEMORY_ORDER_RELAXED);
  if (vkr_atomic_uint32_load(&load->done, VKR_MEMORY_ORDER_ACQUIRE) ||
      perf->retired_count >= VKR_EDITOR_PERF_RETIRED) {
    perf_load_drop(load);
    return;
  }
  perf->retired[perf->retired_count++] = load;
}

/* Frees the given-up reads that have finished. */
vkr_internal void perf_retired_sweep(VkrEditorRecordingPerf *perf) {
  uint32_t kept = 0u;
  for (uint32_t i = 0u; i < perf->retired_count; ++i) {
    VkrEditorPerfLoad *load = perf->retired[i];
    if (vkr_atomic_uint32_load(&load->done, VKR_MEMORY_ORDER_ACQUIRE)) {
      perf_load_drop(load);
    } else {
      perf->retired[kept++] = load;
    }
  }
  perf->retired_count = kept;
}

void vkr_editor_recording_perf_cancel(VkrEditorRecordingPerf *perf) {
  perf_load_abandon(perf, perf->loading);
  perf->loading = NULL;
}

/* Shows a read recording in place of the shown one, keeping the window's
   layout, its tables and the reads still finishing. */
vkr_internal void perf_adopt(VkrEditorUi *editor, VkrEditorPerfLoad *load) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrEditorPerfLoad *retired[VKR_EDITOR_PERF_RETIRED];
  const uint32_t retired_count = perf->retired_count;
  MemCopy(retired, perf->retired, sizeof(retired));
  perf->retired_count = 0u;
  vkr_editor_recording_perf_release(perf);
  const VkrEditorRecordingPerfView view = perf->view;
  MemZero(perf, sizeof(*perf));
  perf->view = view;
  MemCopy(perf->retired, retired, sizeof(retired));
  perf->retired_count = retired_count;
  perf->selected = UINT64_MAX;
  perf->hover_frame = perf->hover_next = UINT64_MAX;
  perf->chart_series = UINT32_MAX;
  perf->view_first = 0u;
  perf->view_last = UINT64_MAX;
  perf->pass_focus = UINT32_MAX;
  perf->source = load;
  perf->allocator = &load->allocator;
  perf->data = load->data;
  perf->analysis = load->analysis;
  perf->loaded = true_v;
  string_format(perf->path, sizeof(perf->path), "%s", load->path);
  perf_bundle_name(load->path, perf->name, sizeof(perf->name));
  perf_columns(perf);
  perf_pass_columns(perf);
}

void vkr_editor_recording_perf_poll(VkrEditorUi *editor) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  perf_retired_sweep(perf);
  VkrEditorPerfLoad *load = perf->loading;
  if (!load || !vkr_atomic_uint32_load(&load->done, VKR_MEMORY_ORDER_ACQUIRE)) {
    return;
  }
  perf->loading = NULL;
  if (load->queued) {
    (void)vkr_job_wait(load->jobs, load->job);
    load->queued = false_v;
  }
  if (!load->ok) {
    string_format(perf->error, sizeof(perf->error), "%s", load->error);
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, vkr_ui_theme()->warning,
                     load->error);
    perf_load_free(load);
    return;
  }
  perf_adopt(editor, load);
}

const char *
vkr_editor_recording_perf_loading(const VkrEditorRecordingPerf *perf) {
  return perf->loading ? perf->loading->path : NULL;
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
  vkr_editor_window_set_visible(editor, VKR_EDITOR_WINDOW_PERFORMANCE, true_v);
  /* The same recording being read keeps its read. */
  if (perf->loading && string_equals(perf->loading->path, path)) {
    return true_v;
  }
  vkr_editor_recording_perf_cancel(perf);
  perf->error[0] = '\0';
  /* The shown recording keeps its zoom and selection. */
  if (perf->loaded && string_equals(perf->path, path)) {
    return true_v;
  }
  VkrEditorPerfLoad *load = vkr_heap_alloc_zeroed(1u, sizeof(*load));
  if (!load) {
    string_format(error, error_capacity, "Out of memory.");
    return false_v;
  }
  string_format(load->path, sizeof(load->path), "%s", path);
  load->jobs = editor->recording.jobs;
  load->started = vkr_platform_get_absolute_time();
  perf->loading = load;
  /* The payload is copied, so it carries the load's address. */
  VkrEditorPerfLoad *payload = load;
  const VkrJobDesc job = {
      .priority = VKR_JOB_PRIORITY_LOW,
      .type_mask = vkr_job_type_mask_all(),
      .run = perf_load_job,
      .payload = &payload,
      .payload_size = sizeof(payload),
  };
  load->queued = editor->recording.jobs &&
                 vkr_job_submit(editor->recording.jobs, &job, &load->job);
  if (!load->queued) {
    /* Without a job system it reads here. */
    VkrJobContext context = {0};
    (void)perf_load_job(&context, &payload);
  }
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

/* Selects `target` for the inspector, and moves an open playback of this
   recording to it; Watch in the inspector opens one. */
void vkr_editor_perf_select(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                            uint64_t target) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  perf->selected = target;
  if (perf_playback(perf, frame)) {
    (void)vkr_editor_recording_send(
        editor,
        (VkrSampleRecordingRequest){.command = VKR_SAMPLE_RECORDING_SEEK_FRAME,
                                    .frame = target},
        false_v);
  }
}

/* The fewest frames a zoom shows. */
#define PERF_ZOOM_MIN_FRAMES 8u

/* Shows frames [first, last] in every chart and the cards; first > last
   shows the whole recording. */
vkr_internal void perf_zoom(VkrEditorRecordingPerf *perf, uint64_t first,
                            uint64_t last) {
  const uint64_t frames = perf->data.frame_count;
  if (first > last || last >= frames) {
    first = 0u;
    last = frames - 1u;
  }
  if (last - first + 1u < PERF_ZOOM_MIN_FRAMES) {
    const uint64_t middle = first + (last - first) / 2u;
    first = middle > PERF_ZOOM_MIN_FRAMES / 2u
                ? middle - PERF_ZOOM_MIN_FRAMES / 2u
                : 0u;
    last = Min(first + PERF_ZOOM_MIN_FRAMES - 1u, frames - 1u);
  }
  perf->view_first = first;
  perf->view_last = last;
  perf_columns(perf);
  perf_pass_columns(perf);
  if (perf->comparing) {
    perf_compare_columns(perf);
  }
}

/* Whether the charts show less than the whole recording. */
vkr_internal bool8_t perf_zoomed(const VkrEditorRecordingPerf *perf) {
  return perf->view_first > 0u || perf->view_last + 1u < perf->data.frame_count;
}

/* The first shown frame of frame-chart column `column`. */
vkr_internal uint64_t perf_column_start(const VkrEditorRecordingPerf *perf,
                                        uint32_t column) {
  const uint64_t frames = perf->view_last + 1u - perf->view_first;
  return perf->view_first +
         (uint64_t)column * frames / Max(perf->column_count, 1u);
}

/* The column showing `frame`, or VKR_UI_PLOT_NONE outside the shown
   frames. */
vkr_internal uint32_t perf_column_of(const VkrEditorRecordingPerf *perf,
                                     uint64_t frame) {
  if (frame == UINT64_MAX || perf->column_count == 0u ||
      frame < perf->view_first || frame > perf->view_last) {
    return VKR_UI_PLOT_NONE;
  }
  const uint64_t frames = perf->view_last + 1u - perf->view_first;
  return (uint32_t)Min((frame - perf->view_first) * perf->column_count /
                           Max(frames, 1u),
                       (uint64_t)perf->column_count - 1u);
}

/* The pass chart column nearest `frame`, or VKR_UI_PLOT_NONE. Columns are
   in frame order. */
vkr_internal uint32_t perf_pass_column_of(const VkrEditorRecordingPerf *perf,
                                          uint64_t frame) {
  const uint32_t count = perf->pass_column_count;
  if (frame == UINT64_MAX || count == 0u || frame < perf->view_first ||
      frame > perf->view_last) {
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

VkrUiPlacement vkr_editor_perf_cell(uint32_t column, uint32_t row) {
  return (VkrUiPlacement){.column = column,
                          .row = row,
                          .column_span = 1u,
                          .row_span = 1u,
                          .justify = VKR_UI_ALIGN_STRETCH,
                          .align = VKR_UI_ALIGN_STRETCH};
}

VkrUiPanelConfig vkr_editor_perf_grid(VkrUiPlacement placement,
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

void vkr_editor_perf_caption(VkrUiSystem *ui, const char *id, String8 text,
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

uint32_t vkr_editor_perf_segmented(VkrUiSystem *ui, const char *id,
                                   VkrUiPlacement placement,
                                   const char *const *labels,
                                   const char *const *tips, uint32_t count,
                                   uint32_t selected) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiTrack columns[VKR_EDITOR_PERF_SEGMENTS_MAX];
  count = Min(count, (uint32_t)VKR_EDITOR_PERF_SEGMENTS_MAX);
  for (uint32_t i = 0u; i < count; ++i) {
    columns[i] = auto_track;
  }
  VkrUiPanelConfig group =
      vkr_editor_perf_grid(placement, columns, count, &fill, 1u);
  group.placement.justify = VKR_UI_ALIGN_START;
  group.placement.align = VKR_UI_ALIGN_CENTER;
  group.style.padding_pt = (VkrUiEdges){1.0f, 1.0f, 1.0f, 1.0f};
  group.style.gap_pt = 1.0f;
  group.style.background_color = theme->field;
  group.style.border_pt = (VkrUiEdges){1.0f, 1.0f, 1.0f, 1.0f};
  group.style.border_color = theme->border;
  group.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  uint32_t chosen = selected;
  if (!vkr_ui_panel_begin(
          ui, string8_create_from_cstr((const uint8_t *)id, string_length(id)),
          &group)) {
    return chosen;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    const bool8_t on = selected == i;
    VkrUiWidgetConfig option = vkr_ui_widget_config_default();
    option.placement = vkr_editor_perf_cell(i, 0u);
    vkr_editor_ghost_style(&option);
    option.style.font_size_pt = theme->font_caption;
    option.style.padding_pt = (VkrUiEdges){2.0f, 9.0f, 2.0f, 9.0f};
    option.style.corner_radius_pt = (Vec4){3.0f, 3.0f, 3.0f, 3.0f};
    option.style.background_color = on ? theme->raised_hover : (Vec4){0};
    option.style.text_color = on ? theme->text : theme->text_secondary;
    if (tips && tips[i]) {
      option.tooltip = string8_create_from_cstr((const uint8_t *)tips[i],
                                                string_length(tips[i]));
    }
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("option"),
                      string8_create_from_cstr((const uint8_t *)labels[i],
                                               string_length(labels[i])),
                      &option)) {
      chosen = i;
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
  return chosen;
}

/* Samples of the transparent plot a canvas takes the pointer with. */
#define PERF_CANVAS_SAMPLES 2u
static const float32_t PERF_CANVAS_VALUES[PERF_CANVAS_SAMPLES] = {0.0f, 0.0f};
/* Zoom steps of one wheel notch, and the zoom range around the fit. */
#define PERF_CANVAS_WHEEL 1.15f
#define PERF_CANVAS_ZOOM_MIN 0.25f
#define PERF_CANVAS_ZOOM_MAX 8.0f

Vec2 vkr_editor_perf_canvas_point(const VkrEditorPerfCanvas *canvas,
                                  Vec2 world) {
  return (Vec2){
      (world.x - canvas->center.x) * canvas->zoom + canvas->size_pt.x * 0.5f,
      (world.y - canvas->center.y) * canvas->zoom + canvas->size_pt.y * 0.5f};
}

void vkr_editor_perf_canvas_fit(VkrEditorPerfCanvas *canvas, Vec2 min, Vec2 max,
                                float32_t margin_pt) {
  const Vec2 size = {Max(max.x - min.x, 1.0e-3f), Max(max.y - min.y, 1.0e-3f)};
  const Vec2 room = {Max(canvas->size_pt.x - 2.0f * margin_pt, 16.0f),
                     Max(canvas->size_pt.y - 2.0f * margin_pt, 16.0f)};
  canvas->center = (Vec2){(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f};
  canvas->zoom = Min(room.x / size.x, room.y / size.y);
  canvas->fit_zoom = canvas->zoom;
  canvas->fitted = true_v;
}

VkrEditorPerfCanvasInput
vkr_editor_perf_canvas_input(VkrUiSystem *ui, const char *id,
                             VkrEditorPerfCanvas *canvas,
                             VkrUiPlacement placement, String8 tooltip) {
  VkrEditorPerfCanvasInput input = {0};
  const String8 label =
      string8_create_from_cstr((const uint8_t *)id, string_length(id));
  const VkrUiId plot_id = vkr_ui_id_stack_widget_label(&ui->id_stack, label);
  const float32_t scale = Max(ui->content_scale, 0.001f);
  /* The canvas takes its size from its last layout. */
  VkrUiRect rect = {0};
  if (vkr_ui_widget_rect(ui, plot_id, &rect)) {
    canvas->size_pt = (Vec2){rect.width / scale, rect.height / scale};
  }
  const Vec2 pointer_pt = {((float32_t)ui->mouse_x - rect.x) / scale,
                           ((float32_t)ui->mouse_y - rect.y) / scale};
  const VkrUiPlotSeries none = {.values = PERF_CANVAS_VALUES};
  const VkrUiPlotConfig plot = {
      .sample_count = PERF_CANVAS_SAMPLES,
      .y_min = 0.0f,
      .y_max = 1.0f,
      .bars = &none,
      .bar_count = 1u,
      .cursor = VKR_UI_PLOT_NONE,
      .range_first = VKR_UI_PLOT_NONE,
      .range_last = VKR_UI_PLOT_NONE,
  };
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement = placement;
  config.tooltip = tooltip;
  config.cursor =
      canvas->dragging ? VKR_WINDOW_CURSOR_GRABBING : VKR_WINDOW_CURSOR_ARROW;
  const VkrUiPlotResult result = vkr_ui_plot(ui, label, &plot, &config);
  const bool8_t hovered = result.hovered != VKR_UI_PLOT_NONE;
  if (canvas->zoom > 0.0f) {
    input.pointer =
        (Vec2){(pointer_pt.x - canvas->size_pt.x * 0.5f) / canvas->zoom +
                   canvas->center.x,
               (pointer_pt.y - canvas->size_pt.y * 0.5f) / canvas->zoom +
                   canvas->center.y};
  }
  input.hovered = hovered;
  /* The wheel zooms about the point under the pointer, which stays put. */
  if (hovered && ui->mouse_wheel != 0 && canvas->zoom > 0.0f) {
    const float32_t factor =
        ui->mouse_wheel > 0 ? PERF_CANVAS_WHEEL : 1.0f / PERF_CANVAS_WHEEL;
    const float32_t zoom = vkr_clamp_f32(
        canvas->zoom * factor, canvas->fit_zoom * PERF_CANVAS_ZOOM_MIN,
        canvas->fit_zoom * PERF_CANVAS_ZOOM_MAX);
    canvas->center = (Vec2){
        input.pointer.x - (pointer_pt.x - canvas->size_pt.x * 0.5f) / zoom,
        input.pointer.y - (pointer_pt.y - canvas->size_pt.y * 0.5f) / zoom};
    canvas->zoom = zoom;
  }
  /* A drag pans; a press and release without one clicks. */
  if (result.brushing && canvas->zoom > 0.0f) {
    const Vec2 mouse = {(float32_t)ui->mouse_x, (float32_t)ui->mouse_y};
    if (!canvas->dragging) {
      int32_t press_x = ui->mouse_x;
      int32_t press_y = ui->mouse_y;
      input_get_button_press_position(ui->input, BUTTON_LEFT, &press_x,
                                      &press_y);
      canvas->drag_px = (Vec2){(float32_t)press_x, (float32_t)press_y};
      canvas->dragging = true_v;
    }
    canvas->center.x -= (mouse.x - canvas->drag_px.x) / scale / canvas->zoom;
    canvas->center.y -= (mouse.y - canvas->drag_px.y) / scale / canvas->zoom;
    canvas->drag_px = mouse;
  } else {
    canvas->dragging = false_v;
  }
  input.clicked = result.clicked != VKR_UI_PLOT_NONE;
  return input;
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
void vkr_editor_perf_card(VkrEditorUi *editor, VkrUiSystem *ui, uint32_t column,
                          const char *caption, String8 value, Vec4 color,
                          String8 compare, Vec4 compare_color) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {{.value = 14.0f, .unit = VKR_UI_TRACK_PX},
                             {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                             {.value = 14.0f, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig card =
      vkr_editor_perf_grid(vkr_editor_perf_cell(column, 0u), &fill, 1u, rows,
                           compare.length ? 3u : 2u);
  card.style.padding_pt = (VkrUiEdges){6.0f, 10.0f, 6.0f, 10.0f};
  card.style.background_color = theme->raised;
  card.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  card.style.border_color = theme->border;
  card.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  (void)vkr_ui_push_id_u64(ui, column);
  if (vkr_ui_panel_begin(ui, string8_lit("card"), &card)) {
    vkr_editor_perf_caption(ui, "caption",
                            string8_create_from_cstr((const uint8_t *)caption,
                                                     string_length(caption)),
                            vkr_editor_perf_cell(0u, 0u),
                            theme->text_secondary);
    VkrUiWidgetConfig text =
        vkr_editor_text_config(theme->font_emphasis, color);
    text.placement = vkr_editor_perf_cell(0u, 1u);
    text.placement.justify = VKR_UI_ALIGN_START;
    text.placement.align = VKR_UI_ALIGN_CENTER;
    text.text.font = editor->heading_font;
    vkr_ui_label(ui, string8_lit("value"), value, &text);
    if (compare.length) {
      vkr_editor_perf_caption(ui, "compare", compare,
                              vkr_editor_perf_cell(0u, 2u), compare_color);
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
  vkr_editor_perf_card(editor, ui, column, caption, value, color, line,
                       line_color);
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
  /* Zoomed, the frame-time cards describe the shown frames. */
  const bool8_t zoomed = perf_zoomed(perf);
  const VkrRecordingStats *time =
      zoomed ? &perf->view_time : &analysis->frame_time;
  const float64_t low_1pct =
      zoomed ? perf->view_low_1pct : analysis->low_1pct_ms;
  const uint64_t over_budget =
      zoomed ? perf->view_over_budget : analysis->over_budget;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack card = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack wide = {.value = 1.6f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack columns[] = {card, card, card, card, card, card, wide, wide};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig cards = vkr_editor_perf_grid(
      vkr_editor_perf_cell(0u, row), columns, ArrayCount(columns), &fill, 1u);
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
  perf_stat_card(
      editor, ui, 4u, "1% low",
      string8_create_formatted(ui->frame_allocator, "%.2f ms", low_1pct),
      theme->text, VKR_RECORDING_COMPARE_LOW_1PCT, 1.0, "");
  perf_stat_card(editor, ui, 5u, "over 16.7 ms",
                 string8_create_formatted(ui->frame_allocator, "%llu frame%s",
                                          (unsigned long long)over_budget,
                                          over_budget == 1u ? "" : "s"),
                 over_budget ? theme->warning : theme->text,
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
  return string8_create_formatted(
      ui->frame_allocator, "%.*s\nClick to inspect  \xc2\xb7  drag to zoom",
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
  return string8_create_formatted(
      ui->frame_allocator, "%.*s\nClick to inspect  \xc2\xb7  drag to zoom",
      (int32_t)text.length, text.str);
}

/* The frame chart-column `column` stands for, in the frame or the pass
   chart's columns. */
vkr_internal uint64_t perf_plot_frame(const VkrEditorRecordingPerf *perf,
                                      bool8_t passes, uint32_t column) {
  if (passes) {
    return column < perf->pass_column_count ? perf->pass_column_frame[column]
                                            : UINT64_MAX;
  }
  return column < perf->column_count ? perf->column_frame[column] : UINT64_MAX;
}

/* A chart of the shown frames. Every chart marks the selected frame (or the
   playhead), the frame under the pointer on any chart, and a drag in
   progress. A click selects the column's frame for the inspector; a drag
   zooms every chart to its frames. `passes` selects the pass chart's
   columns. */
void vkr_editor_perf_plot(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                          const char *id, VkrUiPlacement placement,
                          VkrUiPlotConfig *plot, bool8_t passes,
                          String8 tooltip) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrRecordingPlayback *playback = perf_playback(perf, frame);
  const uint64_t shown =
      perf->selected != UINT64_MAX ? perf->selected
      : playback ? vkr_recording_playback_frame_at(playback, playback->time)
                 : UINT64_MAX;
  plot->cursor =
      passes ? perf_pass_column_of(perf, shown) : perf_column_of(perf, shown);
  plot->cursor_color = theme->accent;
  plot->hover_color = vkr_ui_color_alpha(theme->text, 0.08f);
  /* A drag on any chart, else the frame under the pointer on another. */
  const uint64_t band_first =
      perf->brushing ? perf->brush_first : perf->hover_frame;
  const uint64_t band_last =
      perf->brushing ? perf->brush_last : perf->hover_frame;
  plot->range_first = passes ? perf_pass_column_of(perf, band_first)
                             : perf_column_of(perf, band_first);
  plot->range_last = passes ? perf_pass_column_of(perf, band_last)
                            : perf_column_of(perf, band_last);
  if (plot->range_first == VKR_UI_PLOT_NONE ||
      plot->range_last == VKR_UI_PLOT_NONE) {
    plot->range_first = plot->range_last = VKR_UI_PLOT_NONE;
  }
  plot->range_color =
      vkr_ui_color_alpha(perf->brushing ? theme->accent : theme->text,
                         perf->brushing ? 0.18f : 0.10f);
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement = placement;
  config.style.background_color = theme->field;
  config.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  config.style.padding_pt = (VkrUiEdges){4.0f, 4.0f, 4.0f, 4.0f};
  /* The tooltip describes the column the pointer was over last build. */
  uint32_t *hovered =
      passes ? &perf->pass_hovered_column : &perf->hovered_column;
  config.tooltip = tooltip.length ? tooltip
                   : passes       ? perf_pass_tooltip(ui, perf, *hovered)
                                  : perf_tooltip(ui, perf, *hovered);
  const VkrUiPlotResult result = vkr_ui_plot(
      ui, string8_create_from_cstr((const uint8_t *)id, string_length(id)),
      plot, &config);
  if (result.hovered != VKR_UI_PLOT_NONE) {
    *hovered = result.hovered;
    perf->hover_next = perf_plot_frame(perf, passes, result.hovered);
  }
  if (result.brushing || result.brushed) {
    const uint64_t first = perf_plot_frame(perf, passes, result.brush_first);
    uint64_t last = perf_plot_frame(perf, passes, result.brush_last);
    /* A frame column ends where the next one starts. */
    if (!passes && result.brush_last + 1u < perf->column_count) {
      last = perf_column_start(perf, result.brush_last + 1u) - 1u;
    } else if (!passes) {
      last = perf->view_last;
    }
    const uint64_t start =
        passes ? first : perf_column_start(perf, result.brush_first);
    perf->brushing = result.brushing;
    perf->brush_first = start;
    perf->brush_last = Max(last, start);
    if (result.brushed) {
      perf->brushing = false_v;
      perf_zoom(perf, start, Max(last, start));
    }
  }
  if (result.clicked != VKR_UI_PLOT_NONE) {
    const uint64_t target = perf_plot_frame(perf, passes, result.clicked);
    if (target != UINT64_MAX) {
      vkr_editor_perf_select(editor, frame, target);
    }
  }
}

/* A chart's title row: its name and, for stacks, a legend. */
vkr_internal void perf_title(VkrUiSystem *ui, const char *id, String8 text,
                             VkrUiPlacement placement, bool8_t legend) {
  const VkrUiTheme *theme = vkr_ui_theme();
  if (!legend) {
    vkr_editor_perf_caption(ui, id, text, placement, theme->text_secondary);
    return;
  }
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack columns[] = {fill, auto_track, auto_track, auto_track,
                                auto_track};
  VkrUiPanelConfig row =
      vkr_editor_perf_grid(placement, columns, ArrayCount(columns), &fill, 1u);
  row.style.gap_pt = theme->space_md;
  if (!vkr_ui_panel_begin(
          ui, string8_create_from_cstr((const uint8_t *)id, string_length(id)),
          &row)) {
    return;
  }
  vkr_editor_perf_caption(ui, "name", text, vkr_editor_perf_cell(0u, 0u),
                          theme->text_secondary);
  for (uint32_t s = 0u; s < VKR_EDITOR_PERF_STAGES; ++s) {
    VkrUiWidgetConfig key =
        vkr_editor_text_config(theme->font_caption, theme->text_secondary);
    key.placement = vkr_editor_perf_cell(1u + s, 0u);
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

/* The frame-time bars against the budget, and B's line while comparing. */
vkr_internal void perf_frame_plot(VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame, const char *id,
                                  VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float64_t budget = perf->analysis.budget_ms;
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
  vkr_editor_perf_plot(editor, frame, id, placement, &plot, false_v,
                       (String8){0});
}

void vkr_editor_perf_chart_title(VkrUiSystem *ui, const char *id,
                                 VkrUiPlacement placement, String8 title,
                                 const char *const *keys, const Vec4 *colors,
                                 uint32_t key_count) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  /* The title over a line of keys, so a narrow chart cuts neither. */
  VkrUiTrack columns[VKR_EDITOR_PERF_SEGMENTS_MAX + 1u];
  key_count = Min(key_count, (uint32_t)VKR_EDITOR_PERF_SEGMENTS_MAX);
  for (uint32_t i = 0u; i < key_count; ++i) {
    columns[i] = auto_track;
  }
  columns[key_count] = fill;
  const VkrUiTrack rows[] = {{.value = 16.0f, .unit = VKR_UI_TRACK_PX},
                             {.value = 16.0f, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig block = vkr_editor_perf_grid(
      placement, columns, key_count + 1u, rows, key_count > 0u ? 2u : 1u);
  block.style.gap_pt = theme->space_md;
  if (!vkr_ui_panel_begin(
          ui, string8_create_from_cstr((const uint8_t *)id, string_length(id)),
          &block)) {
    return;
  }
  VkrUiPlacement name = vkr_editor_perf_cell(0u, 0u);
  name.column_span = key_count + 1u;
  vkr_editor_perf_caption(ui, "name", title, name, theme->text_secondary);
  for (uint32_t i = 0u; i < key_count; ++i) {
    VkrUiWidgetConfig key =
        vkr_editor_text_config(theme->font_caption, theme->text_secondary);
    key.placement = vkr_editor_perf_cell(i, 1u);
    key.placement.justify = VKR_UI_ALIGN_START;
    key.placement.align = VKR_UI_ALIGN_CENTER;
    key.icon = VKR_UI_ICON_DOT;
    key.icon_size_pt = 10.0f;
    key.icon_color = colors[i];
    (void)vkr_ui_push_id_u64(ui, i);
    vkr_ui_label(ui, string8_lit("key"),
                 string8_create_from_cstr((const uint8_t *)keys[i],
                                          string_length(keys[i])),
                 &key);
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

vkr_internal void perf_frame_chart(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   uint32_t row) {
  const VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const float64_t budget = perf->analysis.budget_ms;
  perf_title(
      frame->ui, "frame.title",
      string8_create_formatted(
          frame->ui->frame_allocator,
          "Frame time (ms, %s)  \xc2\xb7  top %.1f ms  \xc2\xb7  dashed at "
          "%.1f ms and twice it%s%s",
          vkr_recording_data_frame_source(&perf->data), perf->frame_ms_top,
          budget, perf->comparing ? "  \xc2\xb7  line: B aligned by " : "",
          perf->comparing ? vkr_recording_align_name(perf->compare.align) : ""),
      vkr_editor_perf_cell(0u, row), false_v);
  perf_frame_plot(editor, frame, "frame.plot",
                  vkr_editor_perf_cell(0u, row + 1u));
}

/* The stage chart's title and its legend, whose keys show or hide their
   stage. */
vkr_internal void perf_stage_title(VkrEditorUi *editor, VkrUiSystem *ui,
                                   VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack columns[] = {fill, auto_track, auto_track, auto_track,
                                auto_track};
  VkrUiPanelConfig row =
      vkr_editor_perf_grid(placement, columns, ArrayCount(columns), &fill, 1u);
  row.style.gap_pt = theme->space_xs;
  if (!vkr_ui_panel_begin(ui, string8_lit("stage.title"), &row)) {
    return;
  }
  vkr_editor_perf_caption(
      ui, "name",
      string8_create_formatted(ui->frame_allocator,
                               "CPU by stage (ms), stacked  \xc2\xb7  "
                               "top %.1f ms",
                               perf->stage_ms_top),
      vkr_editor_perf_cell(0u, 0u), theme->text_secondary);
  for (uint32_t s = 0u; s < VKR_EDITOR_PERF_STAGES; ++s) {
    const bool8_t hidden = perf->view.stage_hidden[s];
    VkrUiWidgetConfig key = vkr_ui_widget_config_default();
    key.placement = vkr_editor_perf_cell(1u + s, 0u);
    key.placement.align = VKR_UI_ALIGN_CENTER;
    vkr_editor_ghost_style(&key);
    key.style.font_size_pt = theme->font_caption;
    key.style.padding_pt = (VkrUiEdges){1.0f, 6.0f, 1.0f, 4.0f};
    key.style.text_color =
        hidden ? theme->text_disabled : theme->text_secondary;
    key.icon = VKR_UI_ICON_DOT;
    key.icon_size_pt = 10.0f;
    key.icon_color = hidden ? theme->text_disabled : PERF_STAGE_COLOR[s];
    key.tooltip = hidden ? string8_lit("Show this stage")
                         : string8_lit("Hide this stage");
    (void)vkr_ui_push_id_u64(ui, s);
    if (vkr_ui_button(
            ui, string8_lit("key"),
            string8_create_from_cstr((const uint8_t *)PERF_STAGE_LABEL[s],
                                     string_length(PERF_STAGE_LABEL[s])),
            &key)) {
      perf->view.stage_hidden[s] = !hidden;
      perf_columns(perf);
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

vkr_internal void perf_stage_chart(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   uint32_t row) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  perf_stage_title(editor, frame->ui, vkr_editor_perf_cell(0u, row));
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
  vkr_editor_perf_plot(editor, frame, "stage.plot",
                       vkr_editor_perf_cell(0u, row + 1u), &plot, false_v,
                       (String8){0});
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
  if (perf_zoomed(perf)) {
    return string8_create_formatted(
        ui->frame_allocator,
        "GPU by pass (ms), stacked  \xc2\xb7  %llu timed frames shown  "
        "\xc2\xb7  top %.1f ms",
        (unsigned long long)perf->pass_view_frames, perf->pass_ms_top);
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
             vkr_editor_perf_cell(0u, row), false_v);
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
  vkr_editor_perf_plot(editor, frame, "pass.plot",
                       vkr_editor_perf_cell(0u, row + 1u), &plot, true_v,
                       (String8){0});
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
  VkrUiPanelConfig row =
      search ? vkr_editor_perf_grid(vkr_editor_perf_cell(column, 0u), columns,
                                    ArrayCount(columns), &fill, 1u)
             : vkr_editor_perf_grid(vkr_editor_perf_cell(column, 0u),
                                    passes_columns, ArrayCount(passes_columns),
                                    &fill, 1u);
  row.style.gap_pt = theme->space_xs;
  if (!vkr_ui_panel_begin(ui, string8_lit("list.title"), &row)) {
    return;
  }
  vkr_editor_perf_caption(ui, "caption", text, vkr_editor_perf_cell(0u, 0u),
                          theme->text_secondary);
  if (search) {
    VkrUiTextEditBuffer edit = {view->series_search, view->series_search_length,
                                sizeof(view->series_search)};
    VkrUiPlacement field = vkr_editor_perf_cell(1u, 0u);
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
    mode.placement = vkr_editor_perf_cell(1u + search + i, 0u);
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
  if (!vkr_editor_table_begin(
          ui, "passes", table, columns, PERF_PASS_COLUMN_COUNT, count,
          vkr_editor_perf_cell(column, 1u), &sorted, &first, &end)) {
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
  if (!vkr_editor_table_begin(
          ui, "series", table, columns, PERF_SERIES_COLUMN_COUNT, count,
          vkr_editor_perf_cell(column, 1u), &sorted, &first, &end)) {
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
    table->selected_key =
        perf->chart_series == UINT32_MAX ? UINT64_MAX : perf->chart_series;
    const VkrEditorTableRow row = {
        .key = index,
        .cells = cells,
        .tooltip = string8_create_formatted(
            allocator,
            "%s  \xc2\xb7  min %.*s, median %.*s, p99 %.*s\nClick to %s",
            series->name, (int)low.length, (const char *)low.str,
            (int)median.length, (const char *)median.str, (int)high.length,
            (const char *)high.str,
            perf->chart_series == index ? "remove its chart"
                                        : "chart it under the memory charts"),
    };
    if (vkr_editor_table_row(ui, table, columns, PERF_SERIES_COLUMN_COUNT, r,
                             &row)) {
      perf->chart_series = perf->chart_series == index ? UINT32_MAX : index;
      perf_columns(perf);
    }
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
  VkrUiPanelConfig cell = vkr_editor_perf_grid(
      vkr_editor_perf_cell(column, 0u), &fill, 1u, rows, ArrayCount(rows));
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
             vkr_editor_perf_cell(0u, 0u), false_v);
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
    vkr_editor_perf_plot(editor, frame, "plot", vkr_editor_perf_cell(0u, 1u),
                         &plot, false_v, (String8){0});
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
  VkrUiPanelConfig pair = vkr_editor_perf_grid(
      vkr_editor_perf_cell(0u, row), columns, ArrayCount(columns), &fill, 1u);
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

/* The series the Series table charted: a line on its own axis, its range
   in the title, and a way to remove it. */
vkr_internal void perf_series_chart(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame,
                                    uint32_t row) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrRecordingSeries *series = &perf->data.series[perf->chart_series];
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack columns[] = {fill, auto_track};
  VkrUiPanelConfig title = vkr_editor_perf_grid(
      vkr_editor_perf_cell(0u, row), columns, ArrayCount(columns), &fill, 1u);
  if (vkr_ui_panel_begin(ui, string8_lit("series.title"), &title)) {
    const bool8_t present = perf->series_range[1] >= perf->series_range[0];
    const String8 low =
        perf_series_value(ui->frame_allocator, series->unit,
                          present ? perf->series_range[0] : 0.0);
    const String8 high =
        perf_series_value(ui->frame_allocator, series->unit,
                          present ? perf->series_range[1] : 0.0);
    vkr_editor_perf_caption(
        ui, "name",
        present ? string8_create_formatted(
                      ui->frame_allocator, "%s%s%s%s  \xc2\xb7  %.*s to %.*s",
                      series->name, series->unit[0] ? " (" : "", series->unit,
                      series->unit[0] ? ")" : "", (int)low.length,
                      (const char *)low.str, (int)high.length,
                      (const char *)high.str)
                : string8_create_formatted(ui->frame_allocator,
                                           "%s: no samples here", series->name),
        vkr_editor_perf_cell(0u, 0u), theme->text_secondary);
    VkrUiWidgetConfig close = vkr_editor_icon_button_config(
        1u, 0u, VKR_UI_ICON_CLOSE, string8_lit("Remove this chart"));
    close.style.min_size_pt = close.style.max_size_pt = (Vec2){18.0f, 18.0f};
    close.style.padding_pt = (VkrUiEdges){2.0f, 2.0f, 2.0f, 2.0f};
    if (vkr_ui_button(ui, string8_lit("close"), (String8){0}, &close)) {
      perf->chart_series = UINT32_MAX;
      (void)vkr_ui_panel_end(ui);
      return;
    }
    (void)vkr_ui_panel_end(ui);
  }
  const float32_t pad =
      Max((perf->series_range[1] - perf->series_range[0]) * 0.1f,
          Max(vkr_abs_f32(perf->series_range[1]) * 0.01f, 1.0e-3f));
  const VkrUiPlotSeries line = {.values = perf->series_values,
                                .color = PERF_PASS_COLOR[4]};
  VkrUiPlotConfig plot = {
      .sample_count = perf->column_count,
      .y_min = perf->series_range[0] - pad,
      .y_max = perf->series_range[1] + pad,
      .lines = &line,
      .line_count = 1u,
      .line_width_pt = 2.0f,
  };
  vkr_editor_perf_plot(editor, frame, "series.plot",
                       vkr_editor_perf_cell(0u, row + 1u), &plot, false_v,
                       (String8){0});
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
             vkr_editor_perf_cell(column, 0u), false_v);
  const uint32_t count =
      Min(analysis->spike_count, (uint32_t)VKR_RECORDING_ANALYSIS_SPIKE_MAX);
  if (count == 0u) {
    vkr_editor_perf_caption(ui, "spikes.none",
                            string8_lit("No spikes: every frame stays near its "
                                        "neighbours."),
                            vkr_editor_perf_cell(column, 1u),
                            theme->text_secondary);
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
  if (!vkr_editor_table_begin(
          ui, "spikes", table, columns, PERF_SPIKE_COLUMN_COUNT, count,
          vkr_editor_perf_cell(column, 1u), &sorted, &first, &end)) {
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
            allocator, "%.*s\nClick to inspect this frame",
            (int)cells[PERF_SPIKE_COLUMN_CAUSE].length,
            (const char *)cells[PERF_SPIKE_COLUMN_CAUSE].str),
    };
    if (vkr_editor_table_row(ui, table, columns, PERF_SPIKE_COLUMN_COUNT, r,
                             &row)) {
      vkr_editor_perf_select(editor, frame, spike->frame);
      if (spike->frame < perf->view_first || spike->frame > perf->view_last) {
        perf_zoom(perf, 1u, 0u);
      }
    }
  }
  vkr_editor_table_end(ui);
}

/* ------------------------------------------------------------------------ */
/* Frame inspector                                                           */

/* Lines of the inspector: a section caption, a label and value, or a note. */
typedef enum PerfLineKind {
  PERF_LINE_SECTION = 0,
  PERF_LINE_VALUE,
  PERF_LINE_NOTE,
} PerfLineKind;

typedef struct PerfLine {
  PerfLineKind kind;
  String8 label;
  String8 value;
  Vec4 color;
  Vec4 dot;
  bool8_t has_dot;
} PerfLine;

#define PERF_INSPECTOR_LINES 64u
/* Room the inspector's lines leave for the list's scrollbar. */
#define PERF_INSPECTOR_GUTTER_PT 12.0f
/* Passes the inspector lists for its frame. */
#define PERF_INSPECTOR_PASSES 10u
/* Markers this near the frame are listed with it. */
#define PERF_INSPECTOR_MARKER_REACH 60u
/* The inspector shows GPU pass times from the nearest frame that has them
   within this many frames. */
#define PERF_INSPECTOR_PASS_REACH 60u

typedef struct PerfLines {
  PerfLine items[PERF_INSPECTOR_LINES];
  uint32_t count;
} PerfLines;

vkr_internal PerfLine *perf_line(PerfLines *lines, PerfLineKind kind,
                                 String8 label, String8 value) {
  if (lines->count >= PERF_INSPECTOR_LINES) {
    return NULL;
  }
  PerfLine *line = &lines->items[lines->count++];
  *line = (PerfLine){.kind = kind,
                     .label = label,
                     .value = value,
                     .color = vkr_ui_theme()->text};
  return line;
}

/* Series the inspector shows when the recording has them, with labels. */
static const struct {
  const char *series;
  const char *label;
} PERF_INSPECTOR_COUNTERS[] = {
    {"gpu.submission", "GPU frame"},
    {"frame.render_scale", "Render scale"},
    {"draw.world.calls_issued", "Draw calls"},
    {"draw.world.commands_issued", "Draw commands"},
    {"visibility.objects_tested", "Objects tested"},
    {"draw.shadow.cascades", "Shadow cascades"},
    {"physics.bodies", "Physics bodies"},
    {"physics.awake_bodies", "Awake bodies"},
    {"physics.contacts", "Contacts"},
};

/* The frame's CPU stages, each with its share of the frame time. */
vkr_internal void perf_inspect_stages(VkrUiSystem *ui,
                                      const VkrEditorRecordingPerf *perf,
                                      uint64_t at, PerfLines *lines) {
  if (!perf->has_stages) {
    return;
  }
  (void)perf_line(lines, PERF_LINE_SECTION, string8_lit("CPU stages"),
                  (String8){0});
  for (uint32_t s = 0u; s < VKR_EDITOR_PERF_STAGES; ++s) {
    const uint32_t index =
        vkr_recording_data_find_series(&perf->data, PERF_STAGE_SERIES[s]);
    float64_t ms = 0.0;
    if (!perf_value(&perf->data, index, at, &ms)) {
      continue;
    }
    /* No share of the frame: with a render thread the stages overlap. */
    PerfLine *line =
        perf_line(lines, PERF_LINE_VALUE,
                  string8_create_from_cstr((const uint8_t *)PERF_STAGE_LABEL[s],
                                           string_length(PERF_STAGE_LABEL[s])),
                  string8_create_formatted(ui->frame_allocator, "%.2f ms", ms));
    if (line) {
      line->dot = PERF_STAGE_COLOR[s];
      line->has_dot = true_v;
    }
  }
}

/* The frame's slowest passes, at the nearest frame with GPU times when the
   recording sampled them. */
vkr_internal void perf_inspect_passes(VkrUiSystem *ui,
                                      const VkrEditorRecordingPerf *perf,
                                      uint64_t at, PerfLines *lines) {
  if (!perf->has_passes) {
    return;
  }
  const VkrRecordingData *data = &perf->data;
  const uint64_t source = perf->pass_gpu
                              ? vkr_recording_data_gpu_frame_near(
                                    data, at, PERF_INSPECTOR_PASS_REACH)
                              : at;
  uint32_t count = 0u;
  const VkrRecordingPassRow *rows =
      source != UINT64_MAX ? vkr_recording_data_pass_rows(data, source, &count)
                           : NULL;
  if (count == 0u) {
    (void)perf_line(lines, PERF_LINE_SECTION,
                    string8_lit("Passes: no times near this frame"),
                    (String8){0});
    return;
  }
  /* The slowest rows, by selection; the total over all of them. */
  uint32_t top[PERF_INSPECTOR_PASSES];
  float32_t top_ms[PERF_INSPECTOR_PASSES];
  uint32_t taken = 0u;
  float32_t total = 0.0f;
  for (uint32_t i = 0u; i < count; ++i) {
    float32_t ms = 0.0f;
    if (!perf_row_ms(perf, &rows[i], &ms)) {
      continue;
    }
    total += ms;
    uint32_t slot = 0u;
    if (taken < PERF_INSPECTOR_PASSES) {
      slot = taken++;
    } else if (ms > top_ms[PERF_INSPECTOR_PASSES - 1u]) {
      slot = PERF_INSPECTOR_PASSES - 1u;
    } else {
      continue;
    }
    while (slot > 0u && top_ms[slot - 1u] < ms) {
      top[slot] = top[slot - 1u];
      top_ms[slot] = top_ms[slot - 1u];
      --slot;
    }
    top[slot] = i;
    top_ms[slot] = ms;
  }
  (void)perf_line(lines, PERF_LINE_SECTION,
                  source == at
                      ? string8_create_formatted(
                            ui->frame_allocator, "%s passes  \xc2\xb7  %.2f ms",
                            perf->pass_gpu ? "GPU" : "CPU encode", total)
                      : string8_create_formatted(
                            ui->frame_allocator,
                            "GPU passes at frame %llu  \xc2\xb7  %.2f ms",
                            (unsigned long long)source, total),
                  (String8){0});
  for (uint32_t r = 0u; r < taken; ++r) {
    const VkrRecordingPassRow *row = &rows[top[r]];
    PerfLine *line = perf_line(
        lines, PERF_LINE_VALUE,
        string8_create_formatted(ui->frame_allocator, "%s",
                                 data->passes[row->pass].name),
        string8_create_formatted(
            ui->frame_allocator, "%.2f ms  \xc2\xb7  %3.0f%%", top_ms[r],
            total > 0.0f ? 100.0f * top_ms[r] / total : 0.0f));
    if (line) {
      line->dot = perf_pass_color(perf, row->pass);
      line->has_dot = true_v;
    }
  }
}

/* The spike the frame belongs to, and what explains it. */
vkr_internal void perf_inspect_spike(VkrUiSystem *ui,
                                     const VkrEditorRecordingPerf *perf,
                                     uint64_t at, PerfLines *lines) {
  const VkrRecordingAnalysis *analysis = &perf->analysis;
  for (uint32_t i = 0u; i < analysis->spike_count; ++i) {
    const VkrRecordingSpike *spike = &analysis->spikes[i];
    if (at < spike->first_frame || at > spike->last_frame) {
      continue;
    }
    (void)perf_line(lines, PERF_LINE_SECTION,
                    string8_create_formatted(ui->frame_allocator,
                                             "Spike #%u  \xc2\xb7  %.1fx its "
                                             "neighbours",
                                             i + 1u, spike->ratio),
                    (String8){0});
    PerfLine *cause = perf_line(
        lines, PERF_LINE_NOTE, perf_spike_cause(ui, perf, spike), (String8){0});
    if (cause) {
      cause->color = vkr_ui_theme()->warning;
    }
    return;
  }
}

/* The counters the recording has, memory and the markers near the frame. */
vkr_internal void perf_inspect_context(VkrUiSystem *ui,
                                       const VkrEditorRecordingPerf *perf,
                                       uint64_t at, PerfLines *lines) {
  const VkrRecordingData *data = &perf->data;
  bool8_t section = false_v;
  for (uint32_t i = 0u; i < ArrayCount(PERF_INSPECTOR_COUNTERS); ++i) {
    const uint32_t index =
        vkr_recording_data_find_series(data, PERF_INSPECTOR_COUNTERS[i].series);
    float64_t value = 0.0;
    if (!perf_value(data, index, at, &value)) {
      continue;
    }
    if (!section) {
      (void)perf_line(lines, PERF_LINE_SECTION, string8_lit("Counters"),
                      (String8){0});
      section = true_v;
    }
    const char *unit = data->series[index].unit;
    (void)perf_line(
        lines, PERF_LINE_VALUE,
        string8_create_from_cstr(
            (const uint8_t *)PERF_INSPECTOR_COUNTERS[i].label,
            string_length(PERF_INSPECTOR_COUNTERS[i].label)),
        string_equals(unit, "ms")
            ? string8_create_formatted(ui->frame_allocator, "%.2f ms", value)
            : perf_series_value(ui->frame_allocator, unit, value));
  }
  const char *const memory_series[] = {
      "process.resident_bytes", "memory.gpu.driver.current_allocated_bytes"};
  const char *const memory_labels[] = {"RAM", "VRAM"};
  section = false_v;
  for (uint32_t i = 0u; i < ArrayCount(memory_series); ++i) {
    const uint32_t index =
        vkr_recording_data_find_series(data, memory_series[i]);
    float64_t bytes = 0.0;
    if (!perf_value(data, index, at, &bytes)) {
      continue;
    }
    if (!section) {
      (void)perf_line(lines, PERF_LINE_SECTION, string8_lit("Memory"),
                      (String8){0});
      section = true_v;
    }
    (void)perf_line(lines, PERF_LINE_VALUE,
                    string8_create_from_cstr((const uint8_t *)memory_labels[i],
                                             string_length(memory_labels[i])),
                    perf_series_value(ui->frame_allocator, "bytes", bytes));
  }
  section = false_v;
  for (uint32_t m = 0u; m < data->marker_count; ++m) {
    const VkrRecordingDataMarker *marker = &data->markers[m];
    const uint64_t distance =
        marker->frame > at ? marker->frame - at : at - marker->frame;
    if (distance > PERF_INSPECTOR_MARKER_REACH) {
      continue;
    }
    if (!section) {
      (void)perf_line(lines, PERF_LINE_SECTION, string8_lit("Markers nearby"),
                      (String8){0});
      section = true_v;
    }
    (void)perf_line(
        lines, PERF_LINE_VALUE,
        string8_create_formatted(ui->frame_allocator, "%s", marker->label),
        string8_create_formatted(ui->frame_allocator, "%+lld frames",
                                 (long long)marker->frame - (long long)at));
  }
}

/* One inspector line in row `row` of the open list. */
vkr_internal void perf_inspector_row(VkrEditorUi *editor, VkrUiSystem *ui,
                                     const PerfLine *line, uint32_t row) {
  const VkrUiTheme *theme = vkr_ui_theme();
  (void)vkr_ui_push_id_u64(ui, row);
  if (line->kind != PERF_LINE_VALUE) {
    VkrUiWidgetConfig text = vkr_editor_text_config(
        theme->font_caption,
        line->kind == PERF_LINE_SECTION ? theme->text_secondary : line->color);
    text.placement = vkr_editor_perf_cell(0u, row);
    text.placement.justify = VKR_UI_ALIGN_START;
    text.placement.align = VKR_UI_ALIGN_END;
    /* Clear of the list's scrollbar. */
    text.placement.margin_pt.right = PERF_INSPECTOR_GUTTER_PT;
    if (line->kind == PERF_LINE_SECTION) {
      text.text.font = editor->heading_font;
    }
    text.tooltip = line->label;
    vkr_ui_label(ui, string8_lit("text"), line->label, &text);
    (void)vkr_ui_pop_id(ui);
    return;
  }
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack columns[] = {fill, auto_track};
  VkrUiPanelConfig pair = vkr_editor_perf_grid(
      vkr_editor_perf_cell(0u, row), columns, ArrayCount(columns), &fill, 1u);
  pair.style.gap_pt = theme->space_sm;
  pair.style.padding_pt.right = PERF_INSPECTOR_GUTTER_PT;
  if (vkr_ui_panel_begin(ui, string8_lit("pair"), &pair)) {
    VkrUiWidgetConfig label =
        vkr_editor_text_config(theme->font_caption, theme->text);
    label.placement = vkr_editor_perf_cell(0u, 0u);
    label.placement.justify = VKR_UI_ALIGN_START;
    label.placement.align = VKR_UI_ALIGN_CENTER;
    if (line->has_dot) {
      label.icon = VKR_UI_ICON_DOT;
      label.icon_size_pt = 10.0f;
      label.icon_color = line->dot;
    }
    label.tooltip = line->label;
    vkr_ui_label(ui, string8_lit("label"), line->label, &label);
    VkrUiWidgetConfig value =
        vkr_editor_text_config(theme->font_caption, line->color);
    value.placement = vkr_editor_perf_cell(1u, 0u);
    value.placement.justify = VKR_UI_ALIGN_END;
    value.placement.align = VKR_UI_ALIGN_CENTER;
    vkr_ui_label(ui, string8_lit("value"), line->value, &value);
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_pop_id(ui);
}

/* The spike before or after `at`, or UINT64_MAX. */
vkr_internal uint64_t perf_spike_near(const VkrEditorRecordingPerf *perf,
                                      uint64_t at, bool8_t forward) {
  uint64_t best = UINT64_MAX;
  for (uint32_t i = 0u; i < perf->analysis.spike_count; ++i) {
    const uint64_t candidate = perf->analysis.spikes[i].frame;
    if (forward ? candidate > at && (best == UINT64_MAX || candidate < best)
                : candidate < at && (best == UINT64_MAX || candidate > best)) {
      best = candidate;
    }
  }
  return best;
}

/* The inspector's title: the frame and its time, steps to the previous and
   next frame and spike, and Watch. */
vkr_internal void perf_inspector_title(VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       VkrUiPlacement placement, uint64_t at,
                                       bool8_t selected) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack columns[] = {fill,       auto_track, auto_track,
                                auto_track, auto_track, auto_track};
  VkrUiPanelConfig row =
      vkr_editor_perf_grid(placement, columns, ArrayCount(columns), &fill, 1u);
  row.style.gap_pt = theme->space_xs;
  if (!vkr_ui_panel_begin(ui, string8_lit("inspector.title"), &row)) {
    return;
  }
  VkrUiWidgetConfig name =
      vkr_editor_text_config(theme->font_body, theme->text);
  name.placement = vkr_editor_perf_cell(0u, 0u);
  name.placement.justify = VKR_UI_ALIGN_START;
  name.placement.align = VKR_UI_ALIGN_CENTER;
  name.text.font = editor->heading_font;
  name.tooltip = selected ? string8_lit("The selected frame")
                          : string8_lit("The frame under the pointer; click a "
                                        "chart to keep one");
  vkr_ui_label(ui, string8_lit("name"),
               string8_create_formatted(ui->frame_allocator, "Frame %llu",
                                        (unsigned long long)at),
               &name);
  if (!selected) {
    name.style.text_color = theme->text_secondary;
  }
  const uint64_t last = perf->data.frame_count - 1u;
  const struct {
    VkrUiIcon icon;
    const char *tip;
    uint64_t target;
  } steps[] = {
      {VKR_UI_ICON_ARROW_LEFT, "Previous spike",
       perf_spike_near(perf, at, false_v)},
      {VKR_UI_ICON_CHEVRON_LEFT, "Previous frame",
       at > 0u ? at - 1u : UINT64_MAX},
      {VKR_UI_ICON_CHEVRON_RIGHT, "Next frame",
       at < last ? at + 1u : UINT64_MAX},
      {VKR_UI_ICON_ARROW_RIGHT, "Next spike",
       perf_spike_near(perf, at, true_v)},
  };
  for (uint32_t i = 0u; i < ArrayCount(steps); ++i) {
    VkrUiWidgetConfig step = vkr_editor_icon_button_config(
        1u + i, 0u, steps[i].icon,
        string8_create_from_cstr((const uint8_t *)steps[i].tip,
                                 string_length(steps[i].tip)));
    step.disabled = steps[i].target == UINT64_MAX;
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("step"), (String8){0}, &step)) {
      vkr_editor_perf_select(editor, frame, steps[i].target);
      /* A step past the shown frames moves the view with it. */
      if (steps[i].target < perf->view_first ||
          steps[i].target > perf->view_last) {
        const uint64_t span = perf->view_last - perf->view_first;
        const uint64_t first =
            steps[i].target > span / 2u ? steps[i].target - span / 2u : 0u;
        perf_zoom(perf, first, Min(first + span, last));
      }
    }
    (void)vkr_ui_pop_id(ui);
  }
  VkrUiWidgetConfig watch = vkr_ui_widget_config_default();
  watch.placement = vkr_editor_perf_cell(5u, 0u);
  watch.placement.align = VKR_UI_ALIGN_CENTER;
  vkr_editor_action_style(&watch, editor->heading_font);
  watch.icon = VKR_UI_ICON_PLAY;
  watch.icon_size_pt = 11.0f;
  vkr_editor_button_size(&watch);
  watch.tooltip = string8_lit("Watch this frame in the Scene");
  if (vkr_ui_button(ui, string8_lit("watch"), string8_lit("Watch"), &watch)) {
    perf_seek(editor, frame, at);
  }
  (void)vkr_ui_panel_end(ui);
}

/* The frame inspector: everything recorded about the selected frame, or the
   one under the pointer while nothing is selected. */
vkr_internal void perf_inspector(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const bool8_t selected = perf->selected < perf->data.frame_count;
  const uint64_t at = selected ? perf->selected : perf->hover_frame;
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.value = theme->control_height, .unit = VKR_UI_TRACK_PX}, fill};
  VkrUiPanelConfig panel = vkr_editor_perf_grid(placement, &fill, 1u, rows, 2u);
  panel.style.padding_pt = (VkrUiEdges){8.0f, 10.0f, 8.0f, 10.0f};
  panel.style.gap_pt = theme->space_sm;
  panel.style.background_color = theme->raised;
  panel.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  panel.style.border_color = theme->border;
  panel.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  if (!vkr_ui_panel_begin(ui, string8_lit("inspector"), &panel)) {
    return;
  }
  if (at >= perf->data.frame_count) {
    VkrUiWidgetConfig hint =
        vkr_editor_text_config(theme->font_body, theme->text_secondary);
    hint.placement = vkr_editor_perf_cell(0u, 0u);
    hint.placement.row_span = 2u;
    hint.placement.justify = VKR_UI_ALIGN_CENTER;
    hint.placement.align = VKR_UI_ALIGN_CENTER;
    hint.text.layout.word_wrap = true_v;
    hint.text.layout.max_width = 220.0f;
    hint.text.layout.anchor.horizontal = VKR_TEXT_ALIGN_CENTER;
    vkr_ui_label(ui, string8_lit("hint"),
                 string8_lit("Point at a chart to read a frame; click it or a "
                             "spike to keep it here. Drag across a chart to "
                             "zoom."),
                 &hint);
    (void)vkr_ui_panel_end(ui);
    return;
  }
  perf_inspector_title(editor, frame, vkr_editor_perf_cell(0u, 0u), at,
                       selected);
  PerfLines *lines = vkr_allocator_alloc(ui->frame_allocator, sizeof(*lines),
                                         VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (lines) {
    lines->count = 0u;
    float64_t frame_ms = 0.0;
    const bool8_t timed =
        vkr_recording_data_frame_ms(&perf->data, at, &frame_ms);
    const float64_t budget = perf->analysis.budget_ms;
    PerfLine *time =
        perf_line(lines, PERF_LINE_VALUE, string8_lit("Frame time"),
                  timed ? string8_create_formatted(ui->frame_allocator,
                                                   "%.2f ms", frame_ms)
                        : string8_lit("not measured"));
    if (time && timed) {
      time->color = frame_ms > 2.0 * budget ? theme->error
                    : frame_ms > budget     ? theme->warning
                                            : theme->success;
    }
    (void)perf_line(lines, PERF_LINE_VALUE, string8_lit("Time"),
                    string8_create_formatted(ui->frame_allocator, "%.3f s",
                                             perf->data.times[at]));
    /* With the presenter (ADR-111) some frames only present the UI over
       the last Scene: they built no render graph. */
    const uint32_t graph =
        vkr_recording_data_find_series(&perf->data, "cpu.graph_build");
    float64_t built = 0.0;
    if (graph != UINT32_MAX && !perf_value(&perf->data, graph, at, &built)) {
      PerfLine *note =
          perf_line(lines, PERF_LINE_NOTE,
                    string8_lit("UI-only frame: it presented the last Scene"),
                    (String8){0});
      if (note) {
        note->color = theme->text_secondary;
      }
    }
    perf_inspect_spike(ui, perf, at, lines);
    perf_inspect_stages(ui, perf, at, lines);
    perf_inspect_passes(ui, perf, at, lines);
    perf_inspect_context(ui, perf, at, lines);
    VkrUiTrack line_rows[PERF_INSPECTOR_LINES];
    for (uint32_t i = 0u; i < lines->count; ++i) {
      line_rows[i] = (VkrUiTrack){
          .value = lines->items[i].kind == PERF_LINE_SECTION ? 24.0f : 18.0f,
          .unit = VKR_UI_TRACK_PX};
    }
    VkrUiPanelConfig list = vkr_editor_perf_grid(
        vkr_editor_perf_cell(0u, 1u), &fill, 1u, line_rows, lines->count);
    if (lines->count > 0u &&
        vkr_ui_scroll_area_begin(ui, string8_lit("lines"), &list)) {
      for (uint32_t i = 0u; i < lines->count; ++i) {
        perf_inspector_row(editor, ui, &lines->items[i], i);
      }
      (void)vkr_ui_scroll_area_end(ui);
    }
  }
  (void)vkr_ui_panel_end(ui);
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
  VkrUiPanelConfig body =
      vkr_editor_perf_grid(vkr_editor_perf_cell(0u, 0u), &fill, 1u, &fill, 1u);
  if (!vkr_ui_panel_begin(ui, string8_lit("perf.empty"), &body)) {
    return;
  }
  VkrUiPanelConfig stack = vkr_editor_perf_grid(
      vkr_editor_perf_cell(0u, 0u), &auto_track, 1u, rows, ArrayCount(rows));
  stack.placement.justify = stack.placement.align = VKR_UI_ALIGN_CENTER;
  stack.style.gap_pt = theme->space_md;
  if (vkr_ui_panel_begin(ui, string8_lit("stack"), &stack)) {
    VkrUiWidgetConfig icon = vkr_ui_widget_config_default();
    icon.placement = vkr_editor_perf_cell(0u, 0u);
    icon.placement.justify = VKR_UI_ALIGN_CENTER;
    icon.icon = VKR_UI_ICON_CHART_BAR;
    icon.icon_size_pt = 32.0f;
    icon.icon_color = theme->text_disabled;
    vkr_ui_label(ui, string8_lit("icon"), (String8){0}, &icon);
    VkrUiWidgetConfig title =
        vkr_editor_text_config(theme->font_body, theme->text);
    title.placement = vkr_editor_perf_cell(0u, 1u);
    title.placement.justify = VKR_UI_ALIGN_CENTER;
    title.text.font = editor->heading_font;
    vkr_ui_label(ui, string8_lit("title"), string8_lit("No recording analyzed"),
                 &title);
    VkrUiWidgetConfig text =
        vkr_editor_text_config(theme->font_body, theme->text_secondary);
    text.placement = vkr_editor_perf_cell(0u, 2u);
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
    analyze.placement = vkr_editor_perf_cell(0u, 3u);
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

/* A recording being read with nothing shown yet: its name, the time
   spent, a bar that sweeps while a worker reads, and Cancel. The editor
   stays usable meanwhile. */
vkr_internal void perf_reading(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrEditorPerfLoad *load = perf->loading;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const float32_t track_pt = 260.0f;
  const VkrUiTrack column = {.value = track_pt, .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack rows[] = {auto_track,
                             auto_track,
                             auto_track,
                             {.value = 4.0f, .unit = VKR_UI_TRACK_PX},
                             auto_track};
  VkrUiPanelConfig body =
      vkr_editor_perf_grid(vkr_editor_perf_cell(0u, 0u), &fill, 1u, &fill, 1u);
  if (!vkr_ui_panel_begin(ui, string8_lit("perf.reading"), &body)) {
    return;
  }
  VkrUiPanelConfig stack = vkr_editor_perf_grid(
      vkr_editor_perf_cell(0u, 0u), &column, 1u, rows, ArrayCount(rows));
  stack.placement.justify = stack.placement.align = VKR_UI_ALIGN_CENTER;
  stack.style.gap_pt = theme->space_md;
  if (vkr_ui_panel_begin(ui, string8_lit("stack"), &stack)) {
    VkrUiWidgetConfig icon = vkr_ui_widget_config_default();
    icon.placement = vkr_editor_perf_cell(0u, 0u);
    icon.placement.justify = VKR_UI_ALIGN_CENTER;
    icon.icon = VKR_UI_ICON_SPINNER;
    icon.icon_size_pt = 28.0f;
    icon.icon_color = theme->accent_hover;
    vkr_ui_label(ui, string8_lit("icon"), (String8){0}, &icon);
    char name[VKR_RECORDING_LABEL_MAX + 1u];
    perf_bundle_name(load->path, name, sizeof(name));
    VkrUiWidgetConfig title =
        vkr_editor_text_config(theme->font_body, theme->text);
    title.placement = vkr_editor_perf_cell(0u, 1u);
    title.placement.justify = VKR_UI_ALIGN_CENTER;
    title.text.font = editor->heading_font;
    title.tooltip =
        string8_create_formatted(ui->frame_allocator, "%s", load->path);
    vkr_ui_label(
        ui, string8_lit("title"),
        string8_create_formatted(ui->frame_allocator, "Reading %s", name),
        &title);
    const float64_t elapsed = vkr_platform_get_absolute_time() - load->started;
    VkrUiWidgetConfig text =
        vkr_editor_text_config(theme->font_caption, theme->text_secondary);
    text.placement = vkr_editor_perf_cell(0u, 2u);
    text.placement.justify = VKR_UI_ALIGN_CENTER;
    text.text.layout.word_wrap = true_v;
    text.text.layout.max_width = track_pt;
    text.text.layout.anchor.horizontal = VKR_TEXT_ALIGN_CENTER;
    vkr_ui_label(ui, string8_lit("text"),
                 string8_create_formatted(
                     ui->frame_allocator,
                     "Decoding its frames and finding spikes  \xc2\xb7  %.1f s",
                     elapsed),
                 &text);
    /* No share of the work is known, so a segment sweeps the track. */
    VkrUiWidgetConfig track = vkr_ui_widget_config_default();
    track.placement = vkr_editor_perf_cell(0u, 3u);
    track.placement.justify = VKR_UI_ALIGN_START;
    track.placement.align = VKR_UI_ALIGN_CENTER;
    track.style.min_size_pt = track.style.max_size_pt = (Vec2){track_pt, 4.0f};
    track.style.background_color = vkr_ui_color_alpha(theme->text, 0.12f);
    track.style.corner_radius_pt = (Vec4){2.0f, 2.0f, 2.0f, 2.0f};
    vkr_ui_label(ui, string8_lit("track"), (String8){0}, &track);
    const float32_t segment_pt = 0.3f * track_pt;
    const float32_t phase =
        0.5f - 0.5f * vkr_cos_f32((float32_t)(elapsed * 2.4));
    VkrUiWidgetConfig bar = track;
    bar.placement.justify = VKR_UI_ALIGN_START;
    bar.placement.margin_pt.left = phase * (track_pt - segment_pt);
    bar.style.min_size_pt = bar.style.max_size_pt = (Vec2){segment_pt, 4.0f};
    bar.style.background_color = theme->accent;
    vkr_ui_label(ui, string8_lit("bar"), (String8){0}, &bar);
    VkrUiWidgetConfig cancel = vkr_ui_widget_config_default();
    cancel.placement = vkr_editor_perf_cell(0u, 4u);
    cancel.placement.justify = VKR_UI_ALIGN_CENTER;
    cancel.style.margin_pt.top = theme->space_sm;
    vkr_editor_ghost_style(&cancel);
    vkr_editor_button_size(&cancel);
    cancel.tooltip = perf->loaded
                         ? string8_lit("Stop reading; keep the shown recording")
                         : string8_lit("Stop reading this recording");
    if (vkr_ui_button(ui, string8_lit("cancel"), string8_lit("Cancel"),
                      &cancel)) {
      vkr_editor_recording_perf_cancel(perf);
    }
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

/* The header's sign of another recording being read to replace the shown
   one; a click gives up on it. */
vkr_internal void perf_reading_chip(VkrEditorUi *editor, VkrUiSystem *ui,
                                    uint32_t column) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrUiTheme *theme = vkr_ui_theme();
  char name[VKR_RECORDING_LABEL_MAX + 1u];
  perf_bundle_name(perf->loading->path, name, sizeof(name));
  VkrUiWidgetConfig chip = vkr_ui_widget_config_default();
  chip.placement = vkr_editor_perf_cell(column, 0u);
  chip.placement.align = VKR_UI_ALIGN_CENTER;
  vkr_editor_ghost_style(&chip);
  chip.style.font_size_pt = theme->font_caption;
  chip.style.padding_pt = (VkrUiEdges){2.0f, 8.0f, 2.0f, 6.0f};
  chip.icon = VKR_UI_ICON_SPINNER;
  chip.icon_size_pt = 12.0f;
  chip.icon_color = theme->accent_hover;
  chip.tooltip = string8_lit(
      "Shown here once read and analyzed. Click to stop reading it.");
  if (vkr_ui_button(ui, string8_lit("reading"),
                    string8_create_formatted(ui->frame_allocator,
                                             "Reading %s\xe2\x80\xa6", name),
                    &chip)) {
    vkr_editor_recording_perf_cancel(perf);
  }
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
      vkr_editor_perf_grid(vkr_editor_perf_cell(column, 0u), columns,
                           ArrayCount(columns), &fill, 1u);
  row.placement.justify = VKR_UI_ALIGN_END;
  row.style.gap_pt = theme->space_xs;
  if (!vkr_ui_panel_begin(ui, string8_lit("compare"), &row)) {
    return;
  }
  VkrUiWidgetConfig with =
      vkr_editor_text_config(theme->font_caption, theme->text);
  with.placement = vkr_editor_perf_cell(0u, 0u);
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
    mode.placement = vkr_editor_perf_cell(1u + i, 0u);
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

/* The shown frames and a way back to all of them, or how to zoom. */
vkr_internal void perf_zoom_controls(VkrEditorUi *editor, VkrUiSystem *ui,
                                     uint32_t column) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack columns[] = {auto_track, auto_track};
  const bool8_t zoomed = perf_zoomed(perf);
  VkrUiPanelConfig row = vkr_editor_perf_grid(
      vkr_editor_perf_cell(column, 0u), columns, zoomed ? 2u : 1u, &fill, 1u);
  row.style.gap_pt = theme->space_sm;
  if (!vkr_ui_panel_begin(ui, string8_lit("zoom"), &row)) {
    return;
  }
  const VkrRecordingData *data = &perf->data;
  vkr_editor_perf_caption(
      ui, "range",
      zoomed ? string8_create_formatted(
                   ui->frame_allocator, "Frames %llu to %llu  \xc2\xb7  %.2f s",
                   (unsigned long long)perf->view_first,
                   (unsigned long long)perf->view_last,
                   data->times[perf->view_last] - data->times[perf->view_first])
             : string8_lit("Drag across a chart to zoom"),
      vkr_editor_perf_cell(0u, 0u),
      zoomed ? theme->text : theme->text_disabled);
  if (zoomed) {
    VkrUiWidgetConfig all = vkr_ui_widget_config_default();
    all.placement = vkr_editor_perf_cell(1u, 0u);
    all.placement.align = VKR_UI_ALIGN_CENTER;
    vkr_editor_ghost_style(&all);
    all.style.font_size_pt = theme->font_caption;
    all.style.padding_pt = (VkrUiEdges){2.0f, 8.0f, 2.0f, 6.0f};
    all.icon = VKR_UI_ICON_ZOOM_OUT;
    all.icon_size_pt = 12.0f;
    all.tooltip = string8_lit("Show the whole recording");
    if (vkr_ui_button(ui, string8_lit("all"), string8_lit("Show all"), &all)) {
      perf_zoom(perf, 1u, 0u);
    }
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
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack columns[] = {fill, auto_track, auto_track, auto_track};
  const uint32_t reading_column = 2u;
  const uint32_t compare_column = perf->loading ? 3u : 2u;
  VkrUiPanelConfig header = vkr_editor_perf_grid(
      vkr_editor_perf_cell(0u, row), columns,
      compare_column + (perf->comparing ? 1u : 0u), &fill, 1u);
  header.style.gap_pt = vkr_ui_theme()->space_md;
  if (!vkr_ui_panel_begin(ui, string8_lit("header"), &header)) {
    return;
  }
  perf_zoom_controls(editor, ui, 1u);
  if (perf->loading) {
    perf_reading_chip(editor, ui, reading_column);
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
             vkr_editor_perf_cell(0u, 0u), false_v);
  if (perf->comparing) {
    perf_compare_controls(editor, frame, compare_column);
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
               vkr_editor_perf_cell(column, 0u), false_v);
    return;
  }
  perf_title(ui, "passes.title",
             string8_create_formatted(ui->frame_allocator,
                                      "Passes: mean %s ms per frame, B minus A",
                                      compare->pass_gpu ? "GPU" : "CPU encode"),
             vkr_editor_perf_cell(column, 0u), false_v);
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
  if (!vkr_editor_table_begin(
          ui, "compare.passes", table, columns, PERF_DELTA_COLUMN_COUNT, count,
          vkr_editor_perf_cell(column, 1u), &sorted, &first, &end)) {
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

/* ------------------------------------------------------------------------ */
/* Tabs                                                                      */

static const struct {
  const char *label;
  const char *tip;
  VkrUiIcon icon;
} PERF_TABS[VKR_EDITOR_PERF_TAB_COUNT] = {
    {"Overview",
     "Frame times, CPU stages, passes, spikes and the frame inspector",
     VKR_UI_ICON_CHART_BAR},
    {"Backend",
     "The graphics backend's command buffers, encoders, draws and "
     "dispatches, and the CPU functions that recorded them",
     VKR_UI_ICON_GPU},
    {"Memory",
     "RAM by allocator tag and VRAM by owner, allocations and frees, and "
     "the arenas with their scopes",
     VKR_UI_ICON_MEMORY},
    {"Draws",
     "A map of the level with the camera's path and frustum, what each "
     "frame drew and culled, and its draws by pass and cell",
     VKR_UI_ICON_DRAWS},
    {"Render graph",
     "The render graph's passes in order with their times, and how each "
     "pass flows into the next",
     VKR_UI_ICON_GRAPH},
    {"HW",
     "The GPU: its device facts, the driver's utilization and tiled-scene "
     "figures, and every pipeline with its compile time, threadgroup "
     "limits and register statistics",
     VKR_UI_ICON_GPU},
};

uint64_t vkr_editor_perf_focus(const VkrEditorRecordingPerf *perf,
                               const VkrSampleUiFrame *frame) {
  const VkrRecordingPlayback *playback = perf_playback(perf, frame);
  const uint64_t playhead =
      playback ? vkr_recording_playback_frame_at(playback, playback->time)
               : UINT64_MAX;
  if (playback && playback->playing && playhead < perf->data.frame_count) {
    return playhead;
  }
  if (perf->selected < perf->data.frame_count) {
    return perf->selected;
  }
  if (playhead < perf->data.frame_count) {
    return playhead;
  }
  return perf->hover_frame < perf->data.frame_count ? perf->hover_frame
                                                    : UINT64_MAX;
}

void vkr_editor_perf_empty(VkrEditorUi *editor, VkrUiSystem *ui,
                           VkrUiPlacement placement, VkrUiIcon icon,
                           const char *title, const char *text) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack rows[] = {auto_track, auto_track, auto_track};
  VkrUiPanelConfig body = vkr_editor_perf_grid(placement, &fill, 1u, &fill, 1u);
  /* Keyed by its title: one panel may hold two of these, such as the HW
     tab's utilization and tiled scene charts on a Windows recording. */
  const String8 id =
      string8_create_from_cstr((const uint8_t *)title, string_length(title));
  if (!vkr_ui_panel_begin(ui, id, &body)) {
    return;
  }
  VkrUiPanelConfig stack = vkr_editor_perf_grid(
      vkr_editor_perf_cell(0u, 0u), &auto_track, 1u, rows, ArrayCount(rows));
  stack.placement.justify = stack.placement.align = VKR_UI_ALIGN_CENTER;
  stack.style.gap_pt = theme->space_md;
  if (vkr_ui_panel_begin(ui, string8_lit("stack"), &stack)) {
    VkrUiWidgetConfig mark = vkr_ui_widget_config_default();
    mark.placement = vkr_editor_perf_cell(0u, 0u);
    mark.placement.justify = VKR_UI_ALIGN_CENTER;
    mark.icon = icon;
    mark.icon_size_pt = 32.0f;
    mark.icon_color = theme->text_disabled;
    vkr_ui_label(ui, string8_lit("icon"), (String8){0}, &mark);
    VkrUiWidgetConfig heading =
        vkr_editor_text_config(theme->font_body, theme->text);
    heading.placement = vkr_editor_perf_cell(0u, 1u);
    heading.placement.justify = VKR_UI_ALIGN_CENTER;
    heading.text.font = editor->heading_font;
    vkr_ui_label(
        ui, string8_lit("title"),
        string8_create_from_cstr((const uint8_t *)title, string_length(title)),
        &heading);
    VkrUiWidgetConfig body_text =
        vkr_editor_text_config(theme->font_body, theme->text_secondary);
    body_text.placement = vkr_editor_perf_cell(0u, 2u);
    body_text.placement.justify = VKR_UI_ALIGN_CENTER;
    body_text.text.layout.word_wrap = true_v;
    body_text.text.layout.max_width = 420.0f;
    body_text.text.layout.anchor.horizontal = VKR_TEXT_ALIGN_CENTER;
    vkr_ui_label(
        ui, string8_lit("text"),
        string8_create_from_cstr((const uint8_t *)text, string_length(text)),
        &body_text);
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

/* The tab row: each tab an icon and a name, the shown one in the heading
   font over an accent underline, on a separator across the window. */
vkr_internal void perf_tabs(VkrEditorUi *editor, VkrUiSystem *ui,
                            uint32_t row) {
  VkrEditorRecordingPerfView *view = &editor->recording.perf.view;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiTrack columns[VKR_EDITOR_PERF_TAB_COUNT + 1u];
  for (uint32_t i = 0u; i < VKR_EDITOR_PERF_TAB_COUNT; ++i) {
    columns[i] = auto_track;
  }
  columns[VKR_EDITOR_PERF_TAB_COUNT] = fill;
  VkrUiPanelConfig bar = vkr_editor_perf_grid(
      vkr_editor_perf_cell(0u, row), columns, ArrayCount(columns), &fill, 1u);
  bar.style.gap_pt = theme->space_xs;
  /* A bordered box fills with its border colour under its background, so
     the rule and the underline need the panel's colour over them. */
  bar.style.background_color = theme->panel;
  bar.style.border_pt = (VkrUiEdges){0.0f, 0.0f, 1.0f, 0.0f};
  bar.style.border_color = theme->border;
  if (!vkr_ui_panel_begin(ui, string8_lit("perf.tabs"), &bar)) {
    return;
  }
  for (uint32_t i = 0u; i < VKR_EDITOR_PERF_TAB_COUNT; ++i) {
    const bool8_t active = view->tab == i;
    VkrUiWidgetConfig tab = vkr_ui_widget_config_default();
    tab.placement = vkr_editor_perf_cell(i, 0u);
    tab.placement.justify = VKR_UI_ALIGN_START;
    tab.style.font_size_pt = theme->font_body;
    tab.text.font = active ? editor->heading_font : VKR_FONT_HANDLE_INVALID;
    tab.style.padding_pt = (VkrUiEdges){4.0f, 12.0f, 4.0f, 10.0f};
    tab.style.corner_radius_pt =
        (Vec4){theme->radius, theme->radius, 0.0f, 0.0f};
    tab.style.background_color = theme->panel;
    tab.style.hover_background_color =
        active ? theme->panel
               : vkr_ui_color_mix(theme->panel, theme->raised, 0.6f);
    tab.style.text_color = active ? theme->text : theme->text_secondary;
    tab.style.border_pt =
        active ? (VkrUiEdges){0.0f, 0.0f, 2.0f, 0.0f} : (VkrUiEdges){0};
    tab.style.border_color = theme->accent;
    tab.icon = PERF_TABS[i].icon;
    tab.icon_size_pt = 14.0f;
    tab.icon_color = active ? theme->accent_hover : theme->text_secondary;
    tab.tooltip = string8_create_from_cstr((const uint8_t *)PERF_TABS[i].tip,
                                           string_length(PERF_TABS[i].tip));
    /* The Backend tab takes the name of the backend that recorded. */
    const char *label =
        i == VKR_EDITOR_PERF_TAB_BACKEND
            ? vkr_editor_perf_backend_name(&editor->recording.perf)
            : PERF_TABS[i].label;
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("tab"),
                      string8_create_from_cstr((const uint8_t *)label,
                                               string_length(label)),
                      &tab)) {
      view->tab = i;
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

/* Over every tab but Overview: the frame-time bars of the shown frames,
   which select and zoom as the Overview charts do, beside the frame the tab
   describes with steps to the next frame or spike and Watch. */
vkr_internal void perf_strip(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                             uint32_t row) {
  const VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack columns[] = {fill,
                                {.value = 300.0f, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig strip = vkr_editor_perf_grid(
      vkr_editor_perf_cell(0u, row), columns, ArrayCount(columns), &fill, 1u);
  strip.style.gap_pt = theme->space_md;
  if (!vkr_ui_panel_begin(ui, string8_lit("perf.strip"), &strip)) {
    return;
  }
  perf_frame_plot(editor, frame, "strip.plot", vkr_editor_perf_cell(0u, 0u));
  const uint64_t at = vkr_editor_perf_focus(perf, frame);
  if (at < perf->data.frame_count) {
    VkrUiPlacement title = vkr_editor_perf_cell(1u, 0u);
    title.align = VKR_UI_ALIGN_CENTER;
    perf_inspector_title(editor, frame, title, at,
                         perf->selected < perf->data.frame_count);
  } else {
    VkrUiPlacement hint = vkr_editor_perf_cell(1u, 0u);
    hint.align = VKR_UI_ALIGN_CENTER;
    vkr_editor_perf_caption(
        ui, "strip.hint",
        string8_lit("Click the bars to pick a frame; drag to zoom"), hint,
        theme->text_disabled);
  }
  (void)vkr_ui_panel_end(ui);
}

/* The body of every tab but Overview. */
vkr_internal void perf_tab_body(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame,
                                VkrUiPlacement placement) {
  const VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const uint32_t tab = perf->view.tab;
  if (tab == VKR_EDITOR_PERF_TAB_GRAPH) {
    vkr_editor_recording_perf_graph_build(editor, frame, placement);
    return;
  }
  if (tab == VKR_EDITOR_PERF_TAB_MEMORY) {
    vkr_editor_recording_perf_memory_build(editor, frame, placement);
    return;
  }
  if (tab == VKR_EDITOR_PERF_TAB_BACKEND) {
    vkr_editor_recording_perf_backend_build(editor, frame, placement);
    return;
  }
  if (tab == VKR_EDITOR_PERF_TAB_DRAWS) {
    vkr_editor_recording_perf_draws_build(editor, frame, placement);
    return;
  }
  if (tab == VKR_EDITOR_PERF_TAB_HW) {
    vkr_editor_recording_perf_hw_build(editor, frame, placement);
    return;
  }
  vkr_editor_perf_empty(
      editor, frame->ui, placement, PERF_TABS[tab].icon, PERF_TABS[tab].label,
      "This tab reads data this build of the editor does not record yet.");
}

/* Overview: the cards, the charts beside the frame inspector, and the
   spikes beside the passes, from `row` of the body. */
vkr_internal void perf_overview(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame, VkrUiRect bounds,
                                uint32_t row, float32_t used_height) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrEditorRecordingPerfView *view = &perf->view;
  const float32_t cards_pt = perf->comparing ? 66.0f : 50.0f;
  const float32_t scale = ui->content_scale;
  const float32_t gap = theme->space_sm;
  const VkrUiTrack title = {.value = 18.0f, .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack bar = {.value = 6.0f, .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack rows[] = {
      {.value = cards_pt, .unit = VKR_UI_TRACK_PX},
      {.value = view->charts.ratio, .unit = VKR_UI_TRACK_FR},
      bar,
      {.value = 1.0f - view->charts.ratio, .unit = VKR_UI_TRACK_FR},
  };
  VkrUiPanelConfig body = vkr_editor_perf_grid(
      vkr_editor_perf_cell(0u, row), &fill, 1u, rows, ArrayCount(rows));
  body.style.gap_pt = gap;
  if (!vkr_ui_panel_begin(ui, string8_lit("perf.overview"), &body)) {
    return;
  }
  perf_cards(editor, ui, 0u);

  /* The charts beside the frame inspector, split by a bar. */
  const VkrUiTrack chart_columns[] = {
      {.value = view->inspector.ratio, .unit = VKR_UI_TRACK_FR},
      bar,
      {.value = 1.0f - view->inspector.ratio, .unit = VKR_UI_TRACK_FR}};
  VkrUiPanelConfig top =
      vkr_editor_perf_grid(vkr_editor_perf_cell(0u, 1u), chart_columns,
                           ArrayCount(chart_columns), &fill, 1u);
  top.style.gap_pt = theme->space_md;
  const bool8_t charted = perf->chart_series < perf->data.series_count;
  if (vkr_ui_panel_begin(ui, string8_lit("perf.top"), &top)) {
    const VkrUiTrack chart_rows[] = {
        title,
        {.value = 2.0f, .unit = VKR_UI_TRACK_FR},
        title,
        {.value = 1.2f, .unit = VKR_UI_TRACK_FR},
        title,
        {.value = perf->has_passes ? 1.4f : 0.0f, .unit = VKR_UI_TRACK_FR},
        {.value = 84.0f, .unit = VKR_UI_TRACK_PX},
        title,
        {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
    };
    VkrUiPanelConfig charts = vkr_editor_perf_grid(
        vkr_editor_perf_cell(0u, 0u), &fill, 1u, chart_rows,
        ArrayCount(chart_rows) - (charted ? 0u : 2u));
    charts.style.gap_pt = gap;
    if (vkr_ui_panel_begin(ui, string8_lit("perf.charts"), &charts)) {
      perf_frame_chart(editor, frame, 0u);
      perf_stage_chart(editor, frame, 2u);
      perf_pass_chart(editor, frame, 4u);
      perf_memory_charts(editor, frame, 6u);
      if (charted) {
        perf_series_chart(editor, frame, 7u);
      }
      (void)vkr_ui_panel_end(ui);
    }
    const float32_t inspector_width =
        bounds.width / scale - 24.0f - 6.0f - 2.0f * theme->space_md;
    vkr_editor_split_bar(ui, "perf.split.inspector", &view->inspector,
                         vkr_editor_perf_cell(1u, 0u), false_v,
                         inspector_width);
    perf_inspector(editor, frame, vkr_editor_perf_cell(2u, 0u));
    (void)vkr_ui_panel_end(ui);
  }
  /* The panes' joint lengths: the window's height under the cards, and
     its width, without padding, gaps and bars. */
  const float32_t split_height =
      bounds.height / scale - used_height - cards_pt - 6.0f - 3.0f * gap;
  const float32_t split_width =
      bounds.width / scale - 24.0f - 6.0f - 2.0f * theme->space_md;
  vkr_editor_split_bar(ui, "perf.split.charts", &view->charts,
                       vkr_editor_perf_cell(0u, 2u), true_v, split_height);
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
      vkr_editor_perf_grid(vkr_editor_perf_cell(0u, 3u), columns,
                           ArrayCount(columns), lists, ArrayCount(lists));
  bottom.style.gap_pt = theme->space_md;
  if (vkr_ui_panel_begin(ui, string8_lit("perf.lists"), &bottom)) {
    perf_spikes(editor, frame, 0u);
    VkrUiPlacement split = vkr_editor_perf_cell(1u, 0u);
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

void vkr_editor_recording_perf_build(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     VkrUiRect bounds) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  if (!perf->loaded) {
    if (perf->loading) {
      perf_reading(editor, frame);
    } else {
      perf_empty(editor, frame);
    }
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
  if (view->inspector.ratio <= 0.0f) {
    view->inspector =
        (VkrEditorSplit){.ratio = 0.7f, .min_ratio = 0.5f, .max_ratio = 0.84f};
  }
  view->tab = Min(view->tab, (uint32_t)VKR_EDITOR_PERF_TAB_COUNT - 1u);
  /* Charts report the frame under the pointer as they build; every chart
     and the inspector read the last build's. */
  perf->hover_next = UINT64_MAX;
  const float32_t gap = theme->space_sm;
  const bool8_t overview = view->tab == VKR_EDITOR_PERF_TAB_OVERVIEW;
  const VkrUiTrack title = {.value = 18.0f, .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack tabs = {.value = PERF_TAB_ROW_PT, .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack strip = {.value = PERF_STRIP_PT, .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack overview_rows[] = {title, tabs, fill};
  const VkrUiTrack tab_rows[] = {title, tabs, strip, fill};
  VkrUiPanelConfig body =
      overview ? vkr_editor_perf_grid(vkr_editor_perf_cell(0u, 0u), &fill, 1u,
                                      overview_rows, ArrayCount(overview_rows))
               : vkr_editor_perf_grid(vkr_editor_perf_cell(0u, 0u), &fill, 1u,
                                      tab_rows, ArrayCount(tab_rows));
  body.style.padding_pt = (VkrUiEdges){10.0f, 12.0f, 10.0f, 12.0f};
  body.style.gap_pt = gap;
  body.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("perf.body"), &body)) {
    return;
  }
  perf_header(editor, frame, 0u);
  perf_tabs(editor, ui, 1u);
  if (overview) {
    /* The height above the Overview: padding, title, tabs and their
       gaps. */
    const float32_t used = 20.0f + 18.0f + PERF_TAB_ROW_PT + 2.0f * gap;
    perf_overview(editor, frame, bounds, 2u, used);
  } else {
    perf_strip(editor, frame, 2u);
    perf_tab_body(editor, frame, vkr_editor_perf_cell(0u, 3u));
  }
  (void)vkr_ui_panel_end(ui);
  perf->hover_frame = perf->hover_next;
}
