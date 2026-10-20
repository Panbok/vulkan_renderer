#include "editor_recording.h"

#include "containers/vkr_sort.h"
#include "editor_internal.h"
#include "editor_ui.h"

/* The Performance window's HW tab (ADR-130): the device the recording ran
   on, what its GPU driver reports over time (on Apple GPUs the device,
   renderer and tiler utilization, the tiled scene, the parameter buffer and
   its overflows), and every pipeline the renderer created with what its
   API says about the hardware it needs: threadgroup limits on Metal, and
   registers, spills and instructions on Vulkan when the driver reports
   them. Occupancy and limiter counters of Apple GPUs reach only
   Instruments, which the tab says rather than guessing. */

/* Driver series, as the runtime names them (vkr_standard_scene_runtime.c). */
static const char *const HW_UTIL_SERIES[VKR_EDITOR_PERF_HW_UTILS] = {
    "os.gpu.device_util",
    "os.gpu.renderer_util",
    "os.gpu.tiler_util",
};
static const char *const HW_UTIL_LABEL[VKR_EDITOR_PERF_HW_UTILS] = {
    "Device",
    "Renderer",
    "Tiler",
};
/* The dataviz reference palette's first three slots, as the other tabs. */
static const Vec4 HW_UTIL_COLOR[VKR_EDITOR_PERF_HW_UTILS] = {
    {0.224f, 0.529f, 0.898f, 1.0f},
    {0.851f, 0.349f, 0.149f, 1.0f},
    {0.098f, 0.620f, 0.439f, 1.0f},
};
#define HW_SCENE_SERIES "os.gpu.tiled_scene_bytes"
#define HW_BUFFER_SERIES "os.gpu.parameter_buffer_bytes"
#define HW_SPLIT_SERIES "os.gpu.split_scenes"
#define HW_RECOVERY_SERIES "os.gpu.recoveries"
static const char *const HW_MEMORY_LABEL[] = {"Tiled scene", "Parameter buffer",
                                              "Splits"};
static const Vec4 HW_MEMORY_COLOR[] = {
    {0.224f, 0.529f, 0.898f, 1.0f},
    {0.788f, 0.522f, 0.000f, 1.0f},
    {0.851f, 0.231f, 0.231f, 1.0f},
};

/* Pipeline kinds as VkrPipelineKind numbers them. */
static const char *const HW_KIND_NAME[] = {"render", "compute", "tile"};
#define HW_KIND_COUNT 3u

/* Threads per threadgroup of a device that names none. */
#define HW_DEFAULT_THREADS 1024u

#define HW_MIB (1024.0 * 1024.0)

/* ------------------------------------------------------------------------ */
/* Data                                                                      */

/* The value of hardware fact `key`, or NULL. */
vkr_internal const char *hw_fact(const VkrRecordingData *data,
                                 const char *key) {
  for (uint32_t i = 0u; i < data->hardware_count; ++i) {
    if (string_equals(data->hardware[i].key, key)) {
      return data->hardware[i].value;
    }
  }
  return NULL;
}

/* Series `series`'s value at `frame`, or false without a sample. */
vkr_internal bool8_t hw_value(const VkrRecordingData *data, uint32_t series,
                              uint64_t frame, float64_t *out) {
  if (series == UINT32_MAX || frame >= data->frame_count ||
      !data->series[series].valid[frame]) {
    return false_v;
  }
  *out = data->series[series].values[frame];
  return true_v;
}

vkr_internal void hw_update(VkrEditorRecordingPerf *perf) {
  VkrEditorPerfHw *hw = &perf->hw;
  if (hw->valid && hw->columns_version == perf->columns_version) {
    return;
  }
  const VkrRecordingData *data = &perf->data;
  MemZero(hw, sizeof(*hw));
  hw->columns_version = perf->columns_version;
  hw->valid = true_v;
  for (uint32_t u = 0u; u < VKR_EDITOR_PERF_HW_UTILS; ++u) {
    hw->util_series[u] =
        vkr_recording_data_find_series(data, HW_UTIL_SERIES[u]);
    hw->has_util = hw->has_util || hw->util_series[u] != UINT32_MAX;
  }
  hw->scene_series = vkr_recording_data_find_series(data, HW_SCENE_SERIES);
  hw->buffer_series = vkr_recording_data_find_series(data, HW_BUFFER_SERIES);
  hw->split_series = vkr_recording_data_find_series(data, HW_SPLIT_SERIES);
  hw->recovery_series =
      vkr_recording_data_find_series(data, HW_RECOVERY_SERIES);
  hw->has_scene =
      hw->scene_series != UINT32_MAX || hw->buffer_series != UINT32_MAX;
  const char *threads = hw_fact(data, "Threads per threadgroup");
  int64_t parsed = 0;
  hw->device_threads = threads && string_to_i64(threads, &parsed) && parsed > 0
                           ? (uint32_t)parsed
                           : HW_DEFAULT_THREADS;
  /* Splits count up in the driver, so a column shows those since the
     previous one. */
  float64_t splits_before = -1.0;
  for (uint32_t c = 0u; c < perf->column_count; ++c) {
    const uint64_t f = perf->column_frame[c];
    float64_t value = 0.0;
    for (uint32_t u = 0u; u < VKR_EDITOR_PERF_HW_UTILS; ++u) {
      if (hw_value(data, hw->util_series[u], f, &value)) {
        hw->util[u][c] = (float32_t)value;
      }
    }
    if (hw_value(data, hw->scene_series, f, &value)) {
      hw->scene_mib[c] = (float32_t)(value / HW_MIB);
    }
    if (hw_value(data, hw->buffer_series, f, &value)) {
      hw->buffer_mib[c] = (float32_t)(value / HW_MIB);
    }
    if (hw_value(data, hw->split_series, f, &value)) {
      if (splits_before >= 0.0 && value > splits_before) {
        hw->splits[c] = (float32_t)(value - splits_before);
      }
      splits_before = value;
    }
    hw->memory_top =
        Max(hw->memory_top, Max(hw->scene_mib[c], hw->buffer_mib[c]));
  }
  hw->memory_top = Max(hw->memory_top * 1.1f, 1.0f);
}

/* The mean and largest of series `series` over the shown frames. */
vkr_internal bool8_t hw_shown_stats(const VkrEditorRecordingPerf *perf,
                                    uint32_t series, float64_t *mean,
                                    float64_t *max) {
  const VkrRecordingData *data = &perf->data;
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

/* How much cumulative series `series` grew over the shown frames. */
vkr_internal bool8_t hw_shown_growth(const VkrEditorRecordingPerf *perf,
                                     uint32_t series, float64_t *out) {
  const VkrRecordingData *data = &perf->data;
  if (series == UINT32_MAX) {
    return false_v;
  }
  const uint64_t last = Min(perf->view_last, data->frame_count - 1u);
  float64_t first_value = 0.0;
  float64_t last_value = 0.0;
  bool8_t seen = false_v;
  for (uint64_t f = perf->view_first; f <= last; ++f) {
    if (!data->series[series].valid[f]) {
      continue;
    }
    if (!seen) {
      first_value = data->series[series].values[f];
      seen = true_v;
    }
    last_value = data->series[series].values[f];
  }
  *out = seen ? Max(last_value - first_value, 0.0) : 0.0;
  return seen;
}

/* Compute and tile pipelines whose registers hold their threadgroups under
   the device's limit. */
vkr_internal uint32_t hw_limited(const VkrEditorRecordingPerf *perf) {
  uint32_t count = 0u;
  for (uint32_t i = 0u; i < perf->data.pipeline_count; ++i) {
    const VkrRecordingPipeline *pipeline = &perf->data.pipelines[i];
    count += pipeline->max_threads > 0u &&
                     pipeline->max_threads < perf->hw.device_threads
                 ? 1u
                 : 0u;
  }
  return count;
}

/* ------------------------------------------------------------------------ */
/* Cards                                                                     */

/* A card of a utilization series' mean, its peak on the last line. */
vkr_internal void hw_util_card(VkrEditorUi *editor, VkrUiSystem *ui,
                               uint32_t column, const char *caption,
                               uint32_t series, const char *line) {
  const VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrUiTheme *theme = vkr_ui_theme();
  float64_t mean = 0.0;
  float64_t max = 0.0;
  const bool8_t has = hw_shown_stats(perf, series, &mean, &max);
  vkr_editor_perf_card(
      editor, ui, column, caption,
      has ? string8_create_formatted(ui->frame_allocator, "%.0f%%", mean)
          : string8_lit("not recorded"),
      has ? theme->text : theme->text_disabled,
      has ? string8_create_formatted(ui->frame_allocator,
                                     "mean  \xc2\xb7  peak %.0f%%", max)
          : string8_create_from_cstr((const uint8_t *)line,
                                     string_length(line)),
      theme->text_secondary);
}

vkr_internal void hw_cards(VkrEditorUi *editor, VkrUiSystem *ui,
                           VkrUiPlacement placement) {
  const VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrEditorPerfHw *hw = &perf->hw;
  const VkrRecordingData *data = &perf->data;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack card = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack columns[] = {card, card, card, card, card, card, card};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig row =
      vkr_editor_perf_grid(placement, columns, ArrayCount(columns), &fill, 1u);
  row.style.gap_pt = theme->space_sm;
  if (!vkr_ui_panel_begin(ui, string8_lit("hw.cards"), &row)) {
    return;
  }
  const char *unreported = "the driver reports none";
  hw_util_card(editor, ui, 0u, "GPU busy", hw->util_series[0], unreported);
  hw_util_card(editor, ui, 1u, "Renderer", hw->util_series[1], unreported);
  hw_util_card(editor, ui, 2u, "Tiler", hw->util_series[2], unreported);
  float64_t mean = 0.0;
  float64_t max = 0.0;
  const bool8_t buffer = hw_shown_stats(perf, hw->buffer_series, &mean, &max);
  vkr_editor_perf_card(
      editor, ui, 3u, "Parameter buffer",
      buffer ? string8_create_formatted(ui->frame_allocator, "%.1f MiB",
                                        max / HW_MIB)
             : string8_lit("not recorded"),
      buffer ? theme->text : theme->text_disabled,
      buffer ? string8_lit("largest") : string8_lit("tiled GPUs only"),
      theme->text_secondary);
  float64_t splits = 0.0;
  const bool8_t split = hw_shown_growth(perf, hw->split_series, &splits);
  vkr_editor_perf_card(
      editor, ui, 4u, "Scene splits",
      split ? string8_create_formatted(ui->frame_allocator, "%.0f", splits)
            : string8_lit("not recorded"),
      !split         ? theme->text_disabled
      : splits > 0.0 ? theme->warning
                     : theme->success,
      split ? string8_lit("buffer overflows") : string8_lit("tiled GPUs only"),
      theme->text_secondary);
  float64_t recoveries = 0.0;
  const bool8_t recovered =
      hw_shown_growth(perf, hw->recovery_series, &recoveries);
  vkr_editor_perf_card(
      editor, ui, 5u, "GPU recoveries",
      recovered
          ? string8_create_formatted(ui->frame_allocator, "%.0f", recoveries)
          : string8_lit("not recorded"),
      !recovered         ? theme->text_disabled
      : recoveries > 0.0 ? theme->error
                         : theme->success,
      recovered ? string8_lit("GPU resets") : string8_lit("Apple GPUs only"),
      theme->text_secondary);
  const uint32_t limited = hw_limited(perf);
  vkr_editor_perf_card(
      editor, ui, 6u, "Pipelines",
      data->pipeline_count > 0u
          ? string8_create_formatted(ui->frame_allocator, "%u",
                                     data->pipeline_count)
          : string8_lit("not recorded"),
      data->pipeline_count > 0u ? theme->text : theme->text_disabled,
      data->pipeline_count > 0u
          ? string8_create_formatted(ui->frame_allocator, "%u register-limited",
                                     limited)
          : string8_lit("Tier 1 recordings carry them"),
      limited > 0u ? theme->warning : theme->text_secondary);
  (void)vkr_ui_panel_end(ui);
}

/* ------------------------------------------------------------------------ */
/* Charts                                                                    */

vkr_internal void hw_util_chart(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame,
                                VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrEditorPerfHw *hw = &perf->hw;
  VkrUiSystem *ui = frame->ui;
  if (!hw->has_util) {
    vkr_editor_perf_empty(
        editor, ui, placement, VKR_UI_ICON_GPU, "No GPU utilization",
        string_equals(perf->data.pipeline, "desktop")
            ? "This GPU's driver reports no utilization to the engine; a "
              "vendor profiler such as Radeon GPU Profiler or Nsight "
              "Graphics shows it."
            : "Recordings made on a Mac with Tier 1 metrics carry the Apple "
              "GPU driver's utilization.");
    return;
  }
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.value = VKR_EDITOR_PERF_CHART_TITLE_PT, .unit = VKR_UI_TRACK_PX}, fill};
  VkrUiPanelConfig panel =
      vkr_editor_perf_grid(placement, &fill, 1u, rows, ArrayCount(rows));
  panel.style.gap_pt = vkr_ui_theme()->space_xs;
  if (!vkr_ui_panel_begin(ui, string8_lit("hw.util"), &panel)) {
    return;
  }
  vkr_editor_perf_chart_title(
      ui, "title", vkr_editor_perf_cell(0u, 0u),
      string8_lit("GPU utilization from the driver, %  \xc2\xb7  sampled every "
                  "250 ms"),
      HW_UTIL_LABEL, HW_UTIL_COLOR, VKR_EDITOR_PERF_HW_UTILS);
  VkrUiPlotSeries lines[VKR_EDITOR_PERF_HW_UTILS];
  uint32_t line_count = 0u;
  for (uint32_t u = 0u; u < VKR_EDITOR_PERF_HW_UTILS; ++u) {
    if (hw->util_series[u] == UINT32_MAX) {
      continue;
    }
    lines[line_count++] =
        (VkrUiPlotSeries){.values = hw->util[u], .color = HW_UTIL_COLOR[u]};
  }
  String8 tip = (String8){0};
  const uint32_t c = perf->hovered_column;
  if (c < perf->column_count) {
    tip = string8_create_formatted(ui->frame_allocator, "Frame %llu",
                                   (unsigned long long)perf->column_frame[c]);
    for (uint32_t u = 0u; u < VKR_EDITOR_PERF_HW_UTILS; ++u) {
      tip = string8_create_formatted(ui->frame_allocator, "%.*s\n%s  %.0f%%",
                                     (int32_t)tip.length, tip.str,
                                     HW_UTIL_LABEL[u], hw->util[u][c]);
    }
  }
  VkrUiPlotConfig plot = {
      .sample_count = perf->column_count,
      .y_min = 0.0f,
      /* Headroom, so a GPU busy all the time stays off the top edge. */
      .y_max = 108.0f,
      .lines = lines,
      .line_count = line_count,
  };
  vkr_editor_perf_plot(editor, frame, "util.plot", vkr_editor_perf_cell(0u, 1u),
                       &plot, false_v, tip);
  (void)vkr_ui_panel_end(ui);
}

vkr_internal void hw_scene_chart(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrEditorPerfHw *hw = &perf->hw;
  VkrUiSystem *ui = frame->ui;
  if (!hw->has_scene) {
    vkr_editor_perf_empty(editor, ui, placement, VKR_UI_ICON_LAYERS,
                          "No tiled scene figures",
                          "Apple GPUs report the geometry they bin into "
                          "tiles; other GPUs report none.");
    return;
  }
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.value = VKR_EDITOR_PERF_CHART_TITLE_PT, .unit = VKR_UI_TRACK_PX}, fill};
  VkrUiPanelConfig panel =
      vkr_editor_perf_grid(placement, &fill, 1u, rows, ArrayCount(rows));
  panel.style.gap_pt = vkr_ui_theme()->space_xs;
  if (!vkr_ui_panel_begin(ui, string8_lit("hw.scene"), &panel)) {
    return;
  }
  vkr_editor_perf_chart_title(
      ui, "title", vkr_editor_perf_cell(0u, 0u),
      string8_create_formatted(
          ui->frame_allocator,
          "Tiled geometry, MiB  \xc2\xb7  top %.1f MiB  \xc2\xb7  a split "
          "renders the scene in parts when the buffer fills",
          hw->memory_top),
      HW_MEMORY_LABEL, HW_MEMORY_COLOR, ArrayCount(HW_MEMORY_LABEL));
  /* Splits stand as bars from the floor to a tenth of the chart each, so
     even one shows. */
  float32_t split_bars[VKR_EDITOR_PERF_COLUMNS];
  for (uint32_t c = 0u; c < perf->column_count; ++c) {
    split_bars[c] = hw->splits[c] > 0.0f ? Min(hw->splits[c], 10.0f) * 0.1f *
                                               hw->memory_top * 0.5f
                                         : 0.0f;
  }
  const VkrUiPlotSeries bars[] = {
      {.values = split_bars, .color = HW_MEMORY_COLOR[2]},
  };
  const VkrUiPlotSeries lines[] = {
      {.values = hw->scene_mib, .color = HW_MEMORY_COLOR[0]},
      {.values = hw->buffer_mib, .color = HW_MEMORY_COLOR[1]},
  };
  String8 tip = (String8){0};
  const uint32_t c = perf->hovered_column;
  if (c < perf->column_count) {
    tip = string8_create_formatted(
        ui->frame_allocator,
        "Frame %llu\nTiled scene  %.1f MiB\nParameter buffer  %.1f MiB\nSplits "
        " %.0f",
        (unsigned long long)perf->column_frame[c], hw->scene_mib[c],
        hw->buffer_mib[c], hw->splits[c]);
  }
  VkrUiPlotConfig plot = {
      .sample_count = perf->column_count,
      .y_min = 0.0f,
      .y_max = hw->memory_top,
      .bars = bars,
      .bar_count = ArrayCount(bars),
      .lines = lines,
      .line_count = ArrayCount(lines),
  };
  vkr_editor_perf_plot(editor, frame, "scene.plot",
                       vkr_editor_perf_cell(0u, 1u), &plot, false_v, tip);
  (void)vkr_ui_panel_end(ui);
}

/* ------------------------------------------------------------------------ */
/* Device                                                                    */

static const VkrEditorTableColumn DEVICE_TABLE[] = {
    {.title = "Device", .width_pt = 150.0f, .min_pt = 90.0f},
    {.title = "Value", .width_pt = 0.0f, .min_pt = 90.0f},
};

vkr_internal void hw_device(VkrEditorUi *editor, VkrUiSystem *ui,
                            VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrRecordingData *data = &perf->data;
  const VkrUiTheme *theme = vkr_ui_theme();
  if (data->hardware_count == 0u) {
    vkr_editor_perf_empty(editor, ui, placement, VKR_UI_ICON_GPU,
                          "No device recorded",
                          "Recordings with Tier 1 metrics (the default) "
                          "name the GPU, its memory and its limits.");
    return;
  }
  VkrEditorTable *table = &perf->view.hw_device;
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, "hw.device", table, DEVICE_TABLE,
                              ArrayCount(DEVICE_TABLE), data->hardware_count,
                              placement, &sorted, &first, &end)) {
    return;
  }
  /* The facts keep the order the runtime wrote them in. */
  table->sort_column = VKR_EDITOR_TABLE_NONE;
  for (uint32_t i = first; i < end; ++i) {
    const VkrRecordingHardwareFact *fact = &data->hardware[i];
    const String8 cells[] = {
        string8_create_from_cstr((const uint8_t *)fact->key,
                                 string_length(fact->key)),
        string8_create_from_cstr((const uint8_t *)fact->value,
                                 string_length(fact->value)),
    };
    Vec4 colors[ArrayCount(DEVICE_TABLE)] = {0};
    colors[0] = theme->text_secondary;
    const VkrEditorTableRow row = {
        .key = i,
        .cells = cells,
        .cell_colors = colors,
    };
    (void)vkr_editor_table_row(ui, table, DEVICE_TABLE,
                               ArrayCount(DEVICE_TABLE), i, &row);
  }
  vkr_editor_table_end(ui);
}

/* ------------------------------------------------------------------------ */
/* Pipelines                                                                 */

enum {
  PIPE_NAME = 0,
  PIPE_KIND,
  PIPE_COMPILE,
  PIPE_THREADS,
  PIPE_WIDTH,
  PIPE_MEMORY,
  PIPE_REGISTERS,
  PIPE_SPILL,
  PIPE_INSTRUCTIONS,
  PIPE_FRAME,
  PIPE_COLUMNS,
};

static const VkrEditorTableColumn PIPE_TABLE[PIPE_COLUMNS] = {
    {.title = "Pipeline", .width_pt = 0.0f, .min_pt = 150.0f},
    {.title = "Kind", .width_pt = 62.0f, .min_pt = 48.0f},
    {.title = "Compile ms",
     .width_pt = 100.0f,
     .min_pt = 60.0f,
     .numeric = true_v,
     .tooltip = "Wall time of the create call; the bar compares it with the "
                "slowest"},
    {.title = "Threads",
     .width_pt = 66.0f,
     .min_pt = 52.0f,
     .numeric = true_v,
     .tooltip = "Most threads one threadgroup may hold. Below the device's "
                "limit the kernel's registers cap it: fewer threads share a "
                "core"},
    {.title = "SIMD",
     .width_pt = 50.0f,
     .min_pt = 40.0f,
     .numeric = true_v,
     .tooltip = "Threads that run in lockstep"},
    {.title = "TG memory",
     .width_pt = 76.0f,
     .min_pt = 56.0f,
     .numeric = true_v,
     .tooltip = "Threadgroup or workgroup shared memory it declares"},
    {.title = "Registers",
     .width_pt = 74.0f,
     .min_pt = 56.0f,
     .numeric = true_v,
     .tooltip = "Most registers one of its stages uses, as the driver "
                "reports them (Vulkan with Capture shader statistics)"},
    {.title = "Spill",
     .width_pt = 64.0f,
     .min_pt = 48.0f,
     .numeric = true_v,
     .tooltip = "Scratch or spilled register bytes its stages use"},
    {.title = "Instr.",
     .width_pt = 64.0f,
     .min_pt = 48.0f,
     .numeric = true_v,
     .tooltip = "Instructions of all its stages"},
    {.title = "Frame",
     .width_pt = 60.0f,
     .min_pt = 46.0f,
     .numeric = true_v,
     .tooltip = "The recorded frame that saw it created; 0 also holds those "
                "created before the recording"},
};

typedef struct HwSortItem {
  float64_t key;
  const char *name;
  uint32_t index;
} HwSortItem;

vkr_internal int32_t hw_compare_items(const void *lhs, const void *rhs) {
  const HwSortItem *a = lhs;
  const HwSortItem *b = rhs;
  if (a->name && b->name) {
    const int32_t order = string_compare(a->name, b->name);
    if (order != 0) {
      return order;
    }
  }
  if (a->key != b->key) {
    return a->key < b->key ? -1 : 1;
  }
  return a->index < b->index ? -1 : a->index > b->index ? 1 : 0;
}

vkr_internal float64_t hw_pipeline_key(const VkrRecordingPipeline *pipeline,
                                       uint32_t column, uint32_t index) {
  switch (column) {
  case PIPE_KIND:
    return (float64_t)pipeline->kind;
  case PIPE_COMPILE:
    return pipeline->compile_ms;
  case PIPE_THREADS:
    return pipeline->max_threads;
  case PIPE_WIDTH:
    return pipeline->execution_width;
  case PIPE_MEMORY:
    return pipeline->threadgroup_memory;
  case PIPE_REGISTERS:
    return pipeline->registers;
  case PIPE_SPILL:
    return pipeline->spill_bytes;
  case PIPE_INSTRUCTIONS:
    return pipeline->instructions;
  case PIPE_FRAME:
    return (float64_t)pipeline->frame;
  default:
    return (float64_t)index;
  }
}

/* A count as a cell, or a dash where the API reports none. */
vkr_internal String8 hw_count_cell(VkrAllocator *allocator, uint32_t value) {
  return value > 0u ? string8_create_formatted(allocator, "%u", value)
                    : string8_lit("\xe2\x80\x94");
}

/* Bytes as a cell: KiB from 1 KiB, else bytes; a dash for none. */
vkr_internal String8 hw_bytes_cell(VkrAllocator *allocator, uint32_t value) {
  if (value == 0u) {
    return string8_lit("\xe2\x80\x94");
  }
  return value >= 1024u ? string8_create_formatted(allocator, "%.1f KiB",
                                                   (float64_t)value / 1024.0)
                        : string8_create_formatted(allocator, "%u B", value);
}

/* The kind filter over the table: all, or one VkrPipelineKind. */
vkr_internal bool8_t hw_pipeline_shown(const VkrEditorRecordingPerf *perf,
                                       const VkrRecordingPipeline *pipeline) {
  return perf->view.hw_kind == 0u || pipeline->kind + 1u == perf->view.hw_kind;
}

/* Columns any shown pipeline has a value in: name, kind and compile time
   always; Metal fills no register columns, Vulkan render pipelines no
   threadgroup ones. A bit per column. */
vkr_internal uint32_t hw_pipeline_columns(const VkrEditorRecordingPerf *perf) {
  uint32_t mask = (1u << PIPE_NAME) | (1u << PIPE_KIND) | (1u << PIPE_COMPILE);
  for (uint32_t i = 0u; i < perf->data.pipeline_count; ++i) {
    const VkrRecordingPipeline *pipeline = &perf->data.pipelines[i];
    mask |= pipeline->max_threads > 0u ? 1u << PIPE_THREADS : 0u;
    mask |= pipeline->execution_width > 0u ? 1u << PIPE_WIDTH : 0u;
    mask |= pipeline->threadgroup_memory > 0u ? 1u << PIPE_MEMORY : 0u;
    mask |= pipeline->registers > 0u ? 1u << PIPE_REGISTERS : 0u;
    mask |= pipeline->spill_bytes > 0u ? 1u << PIPE_SPILL : 0u;
    mask |= pipeline->instructions > 0u ? 1u << PIPE_INSTRUCTIONS : 0u;
    mask |= pipeline->frame > 0u ? 1u << PIPE_FRAME : 0u;
  }
  return mask;
}

vkr_internal void hw_pipelines(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame,
                               VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrRecordingData *data = &perf->data;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  if (data->pipeline_count == 0u) {
    vkr_editor_perf_empty(editor, ui, placement, VKR_UI_ICON_GRAPHICS,
                          "No pipelines recorded",
                          "Recordings with Tier 1 metrics (the default) list "
                          "every pipeline the renderer created, with its "
                          "compile time and hardware limits.");
    return;
  }
  /* The shown columns; another set starts the table afresh, since its
     widths and sort follow column positions. */
  const uint32_t mask = hw_pipeline_columns(perf);
  VkrEditorTable *table = &perf->view.hw_pipelines;
  if (perf->view.hw_columns != mask) {
    MemZero(table, sizeof(*table));
    perf->view.hw_columns = mask;
  }
  uint32_t shown[PIPE_COLUMNS];
  VkrEditorTableColumn columns[PIPE_COLUMNS];
  uint32_t shown_count = 0u;
  uint32_t compile_position = 0u;
  for (uint32_t c = 0u; c < PIPE_COLUMNS; ++c) {
    if (mask & (1u << c)) {
      compile_position = c == PIPE_COMPILE ? shown_count : compile_position;
      columns[shown_count] = PIPE_TABLE[c];
      shown[shown_count++] = c;
    }
  }
  HwSortItem *items = vkr_allocator_alloc(
      ui->frame_allocator, sizeof(HwSortItem) * (uint64_t)data->pipeline_count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!items) {
    return;
  }
  uint32_t count = 0u;
  float32_t slowest = 1.0e-3f;
  for (uint32_t i = 0u; i < data->pipeline_count; ++i) {
    const VkrRecordingPipeline *pipeline = &data->pipelines[i];
    if (!hw_pipeline_shown(perf, pipeline)) {
      continue;
    }
    slowest = Max(slowest, pipeline->compile_ms);
    items[count++] = (HwSortItem){.index = i};
  }
  const bool8_t fresh = !table->initialized;
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, "hw.pipelines", table, columns, shown_count,
                              count, placement, &sorted, &first, &end)) {
    return;
  }
  /* The slowest compiles first until a header sorts otherwise. */
  if (fresh) {
    table->sort_column = compile_position;
    table->sort_descending = true_v;
  }
  const uint32_t sort = table->sort_column < shown_count
                            ? shown[table->sort_column]
                            : VKR_EDITOR_TABLE_NONE;
  if (sort != VKR_EDITOR_TABLE_NONE) {
    for (uint32_t r = 0u; r < count; ++r) {
      const VkrRecordingPipeline *pipeline = &data->pipelines[items[r].index];
      items[r].key = hw_pipeline_key(pipeline, sort, items[r].index);
      items[r].name = sort == PIPE_NAME ? pipeline->name : NULL;
    }
    vkr_sort(items, count, sizeof(HwSortItem), hw_compare_items);
    if (table->sort_descending) {
      for (uint32_t r = 0u; r < count / 2u; ++r) {
        const HwSortItem swap = items[r];
        items[r] = items[count - 1u - r];
        items[count - 1u - r] = swap;
      }
    }
  }
  VkrAllocator *allocator = ui->frame_allocator;
  const uint32_t device_threads = perf->hw.device_threads;
  for (uint32_t r = first; r < end; ++r) {
    const uint32_t i = items[r].index;
    const VkrRecordingPipeline *pipeline = &data->pipelines[i];
    const bool8_t limited =
        pipeline->max_threads > 0u && pipeline->max_threads < device_threads;
    const char *kind = pipeline->kind < HW_KIND_COUNT
                           ? HW_KIND_NAME[pipeline->kind]
                           : "\xe2\x80\x94";
    const String8 cells[PIPE_COLUMNS] = {
        string8_create_from_cstr((const uint8_t *)pipeline->name,
                                 string_length(pipeline->name)),
        string8_create_from_cstr((const uint8_t *)kind, string_length(kind)),
        pipeline->compile_ms >= 0.0f
            ? string8_create_formatted(allocator, "%.2f", pipeline->compile_ms)
            : string8_lit("\xe2\x80\x94"),
        hw_count_cell(allocator, pipeline->max_threads),
        hw_count_cell(allocator, pipeline->execution_width),
        hw_bytes_cell(allocator, pipeline->threadgroup_memory),
        hw_count_cell(allocator, pipeline->registers),
        hw_bytes_cell(allocator, pipeline->spill_bytes),
        hw_count_cell(allocator, pipeline->instructions),
        string8_create_formatted(allocator, "%llu",
                                 (unsigned long long)pipeline->frame),
    };
    Vec4 colors[PIPE_COLUMNS] = {0};
    colors[PIPE_KIND] = theme->text_secondary;
    colors[PIPE_FRAME] = theme->text_disabled;
    if (limited) {
      colors[PIPE_THREADS] = theme->warning;
    }
    if (pipeline->spill_bytes > 0u) {
      colors[PIPE_SPILL] = theme->error;
    }
    float32_t bars[PIPE_COLUMNS];
    Vec4 bar_colors[PIPE_COLUMNS] = {0};
    for (uint32_t c = 0u; c < PIPE_COLUMNS; ++c) {
      bars[c] = -1.0f;
    }
    bars[PIPE_COMPILE] = Max(pipeline->compile_ms, 0.0f) / slowest;
    bar_colors[PIPE_COMPILE] = HW_UTIL_COLOR[0];
    if (pipeline->max_threads > 0u) {
      bars[PIPE_THREADS] = Min(
          (float32_t)pipeline->max_threads / (float32_t)device_threads, 1.0f);
      bar_colors[PIPE_THREADS] = limited ? theme->warning : theme->success;
    }
    /* The shown columns' cells, in order. */
    String8 shown_cells[PIPE_COLUMNS];
    Vec4 shown_colors[PIPE_COLUMNS];
    float32_t shown_bars[PIPE_COLUMNS];
    Vec4 shown_bar_colors[PIPE_COLUMNS];
    for (uint32_t c = 0u; c < shown_count; ++c) {
      shown_cells[c] = cells[shown[c]];
      shown_colors[c] = colors[shown[c]];
      shown_bars[c] = bars[shown[c]];
      shown_bar_colors[c] = bar_colors[shown[c]];
    }
    const VkrEditorTableRow row = {
        .key = i,
        .cells = shown_cells,
        .cell_colors = shown_colors,
        .cell_bars = shown_bars,
        .bar_colors = shown_bar_colors,
        .tooltip = limited ? string8_create_formatted(
                                 allocator,
                                 "Its registers hold a threadgroup to %u of "
                                 "the device's %u threads",
                                 pipeline->max_threads, device_threads)
                           : (String8){0},
    };
    (void)vkr_editor_table_row(ui, table, columns, shown_count, r, &row);
  }
  vkr_editor_table_end(ui);
}

static const VkrEditorTableColumn STAT_TABLE[] = {
    {.title = "Driver statistic", .width_pt = 0.0f, .min_pt = 150.0f},
    {.title = "Value", .width_pt = 90.0f, .min_pt = 60.0f, .numeric = true_v},
};

/* The selected pipeline's driver statistics, or what its API reports
   instead. */
vkr_internal void hw_statistics(VkrEditorUi *editor, VkrUiSystem *ui,
                                VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrRecordingData *data = &perf->data;
  const uint64_t selected = perf->view.hw_pipelines.selected_key;
  if (selected >= data->pipeline_count) {
    vkr_editor_perf_empty(editor, ui, placement, VKR_UI_ICON_LIST,
                          "Select a pipeline",
                          "Its driver statistics show here.");
    return;
  }
  const VkrRecordingPipeline *pipeline = &data->pipelines[selected];
  if (pipeline->stat_count == 0u) {
    const bool8_t tiled = string_equals(data->pipeline, "tiled");
    vkr_editor_perf_empty(
        editor, ui, placement, VKR_UI_ICON_INFO_FILL, pipeline->name,
        tiled ? (pipeline->kind == VKR_PIPELINE_KIND_RENDER
                     ? "Metal reports nothing about render stages' "
                       "registers; Xcode's shader profiler shows their cost."
                     : "Metal reports only this kernel's threadgroup limits, "
                       "listed in its row.")
              : "The driver reported no statistics: turn on Capture shader "
                "statistics in Graphics settings and record again.");
    return;
  }
  VkrEditorTable *table = &perf->view.hw_stats;
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, "hw.stats", table, STAT_TABLE,
                              ArrayCount(STAT_TABLE), pipeline->stat_count,
                              placement, &sorted, &first, &end)) {
    return;
  }
  table->sort_column = VKR_EDITOR_TABLE_NONE;
  for (uint32_t i = first; i < end; ++i) {
    const VkrRecordingPipelineStat *stat =
        &data->pipeline_stats[pipeline->stat_first + i];
    const String8 cells[] = {
        string8_create_from_cstr((const uint8_t *)stat->name,
                                 string_length(stat->name)),
        string8_create_formatted(ui->frame_allocator, "%.0f", stat->value),
    };
    const VkrEditorTableRow row = {.key = i, .cells = cells};
    (void)vkr_editor_table_row(ui, table, STAT_TABLE, ArrayCount(STAT_TABLE), i,
                               &row);
  }
  vkr_editor_table_end(ui);
}

/* What the engine cannot read, and where it can be read. */
vkr_internal void hw_note(VkrEditorUi *editor, VkrUiSystem *ui,
                          VkrUiPlacement placement) {
  const VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig note =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  note.placement = placement;
  note.placement.align = VKR_UI_ALIGN_CENTER;
  note.icon = VKR_UI_ICON_INFO_FILL;
  note.icon_size_pt = 12.0f;
  note.icon_color = theme->text_secondary;
  note.text.layout.word_wrap = true_v;
  note.text.layout.max_width = 520.0f;
  vkr_ui_label(
      ui, string8_lit("hw.note"),
      string_equals(perf->data.pipeline, "desktop")
          ? string8_lit("Occupancy and wave counters stay with vendor "
                        "profilers (Radeon GPU Profiler, Nsight Graphics).")
          : string8_lit("Apple GPUs give occupancy and ALU, texture and "
                        "memory limiters only to Instruments (Metal System "
                        "Trace with GPU counters)."),
      &note);
}

/* ------------------------------------------------------------------------ */
/* Tab                                                                       */

void vkr_editor_recording_perf_hw_build(VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame,
                                        VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrRecordingData *data = &perf->data;
  hw_update(perf);
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack title = {.value = 20.0f, .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack rows[] = {{.value = 62.0f, .unit = VKR_UI_TRACK_PX}, fill};
  VkrUiPanelConfig body =
      vkr_editor_perf_grid(placement, &fill, 1u, rows, ArrayCount(rows));
  body.style.gap_pt = theme->space_sm;
  if (!vkr_ui_panel_begin(ui, string8_lit("hw.body"), &body)) {
    return;
  }
  hw_cards(editor, ui, vkr_editor_perf_cell(0u, 0u));
  const VkrUiTrack columns[] = {{.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                                {.value = 1.25f, .unit = VKR_UI_TRACK_FR}};
  VkrUiPanelConfig split = vkr_editor_perf_grid(
      vkr_editor_perf_cell(0u, 1u), columns, ArrayCount(columns), &fill, 1u);
  split.style.gap_pt = theme->space_md;
  if (vkr_ui_panel_begin(ui, string8_lit("hw.split"), &split)) {
    /* Left: the driver over time, then the device. */
    const VkrUiTrack left_rows[] = {
        {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
        {.value = 0.9f, .unit = VKR_UI_TRACK_FR},
        title,
        {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
    };
    VkrUiPanelConfig left =
        vkr_editor_perf_grid(vkr_editor_perf_cell(0u, 0u), &fill, 1u, left_rows,
                             ArrayCount(left_rows));
    left.style.gap_pt = theme->space_sm;
    if (vkr_ui_panel_begin(ui, string8_lit("hw.left"), &left)) {
      hw_util_chart(editor, frame, vkr_editor_perf_cell(0u, 0u));
      hw_scene_chart(editor, frame, vkr_editor_perf_cell(0u, 1u));
      vkr_editor_perf_caption(
          ui, "hw.device.title",
          string8_create_formatted(ui->frame_allocator,
                                   "The %s device the recording ran on",
                                   vkr_editor_perf_backend_name(perf)),
          vkr_editor_perf_cell(0u, 2u), theme->text_secondary);
      hw_device(editor, ui, vkr_editor_perf_cell(0u, 3u));
      (void)vkr_ui_panel_end(ui);
    }
    /* Right: the pipelines, the selected one's statistics and the note. */
    const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
    const VkrUiTrack right_rows[] = {
        {.value = theme->control_height, .unit = VKR_UI_TRACK_PX},
        {.value = 1.7f, .unit = VKR_UI_TRACK_FR},
        title,
        {.value = 0.8f, .unit = VKR_UI_TRACK_FR},
        auto_track,
    };
    VkrUiPanelConfig right =
        vkr_editor_perf_grid(vkr_editor_perf_cell(1u, 0u), &fill, 1u,
                             right_rows, ArrayCount(right_rows));
    right.style.gap_pt = theme->space_xs;
    if (vkr_ui_panel_begin(ui, string8_lit("hw.right"), &right)) {
      const VkrUiTrack head_columns[] = {fill, auto_track};
      VkrUiPanelConfig head =
          vkr_editor_perf_grid(vkr_editor_perf_cell(0u, 0u), head_columns,
                               ArrayCount(head_columns), &fill, 1u);
      head.style.gap_pt = theme->space_md;
      if (vkr_ui_panel_begin(ui, string8_lit("hw.pipelines.head"), &head)) {
        float64_t compile_ms = 0.0;
        for (uint32_t i = 0u; i < data->pipeline_count; ++i) {
          compile_ms += Max(data->pipelines[i].compile_ms, 0.0f);
        }
        vkr_editor_perf_caption(
            ui, "title",
            string8_create_formatted(ui->frame_allocator,
                                     "Pipelines the renderer created  "
                                     "\xc2\xb7  %u  \xc2\xb7  %.0f ms "
                                     "compiling",
                                     data->pipeline_count, compile_ms),
            vkr_editor_perf_cell(0u, 0u), theme->text_secondary);
        static const char *const kinds[] = {"All", "Render", "Compute", "Tile"};
        static const char *const tips[] = {
            "Every pipeline", "Vertex and fragment pipelines",
            "Compute kernels", "Tile shaders of the tiled pipeline"};
        VkrUiPlacement filter = vkr_editor_perf_cell(1u, 0u);
        filter.align = VKR_UI_ALIGN_CENTER;
        perf->view.hw_kind =
            vkr_editor_perf_segmented(ui, "kind", filter, kinds, tips,
                                      ArrayCount(kinds), perf->view.hw_kind);
        (void)vkr_ui_panel_end(ui);
      }
      hw_pipelines(editor, frame, vkr_editor_perf_cell(0u, 1u));
      const uint64_t selected = perf->view.hw_pipelines.selected_key;
      vkr_editor_perf_caption(
          ui, "hw.stats.title",
          selected < data->pipeline_count
              ? string8_create_formatted(ui->frame_allocator,
                                         "Driver statistics of %s",
                                         data->pipelines[selected].name)
              : string8_lit("Driver statistics"),
          vkr_editor_perf_cell(0u, 2u), theme->text_secondary);
      hw_statistics(editor, ui, vkr_editor_perf_cell(0u, 3u));
      hw_note(editor, ui, vkr_editor_perf_cell(0u, 4u));
      (void)vkr_ui_panel_end(ui);
    }
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_panel_end(ui);
}
