#include "editor_recording.h"

#include "containers/vkr_sort.h"
#include "editor_internal.h"
#include "editor_ui.h"

/* The Performance window's Memory tab: RAM by named allocator and VRAM by
   GPU owner over the shown frames, each on its own axis and never summed,
   the allocations and frees each frame, and at the shown frame where the
   memory lives and how the arenas and their scopes stand. */

#define MEMORY_MB (1024.0 * 1024.0)

/* Categorical colours: the dataviz palette's dark steps in their fixed
   order, then a neutral for the rest. */
static const Vec4 MEMORY_COLOR[VKR_EDITOR_PERF_MEMORY_SERIES] = {
    {0.224f, 0.529f, 0.898f, 1.0f}, {0.851f, 0.349f, 0.149f, 1.0f},
    {0.098f, 0.620f, 0.439f, 1.0f}, {0.788f, 0.522f, 0.000f, 1.0f},
    {0.835f, 0.318f, 0.506f, 1.0f}, {0.565f, 0.522f, 0.914f, 1.0f},
    {0.500f, 0.500f, 0.520f, 1.0f},
};
/* Allocations above the line, frees below it. */
static const Vec4 MEMORY_ALLOC_COLOR = {0.224f, 0.529f, 0.898f, 1.0f};
static const Vec4 MEMORY_FREE_COLOR = {0.851f, 0.349f, 0.149f, 1.0f};

#define MEMORY_OWNER_PREFIX "memory.gpu.owner."
#define MEMORY_OWNER_LIVE ".bytes.live"

/* Rows the "where it lives" table holds at most: every allocator id and
   every owner. */
#define MEMORY_WHERE_MAX                                                       \
  (VKR_RECORDING_ALLOCATOR_MAX + VKR_EDITOR_PERF_GPU_OWNERS)

/* ------------------------------------------------------------------------ */
/* Data                                                                      */

vkr_internal bool8_t memory_value(const VkrRecordingData *data, uint32_t series,
                                  uint64_t frame, float64_t *out) {
  if (series == UINT32_MAX || frame >= data->frame_count ||
      !data->series[series].valid[frame]) {
    return false_v;
  }
  *out = data->series[series].values[frame];
  return true_v;
}

/* The series named `prefix` + `owner` + `suffix`, or UINT32_MAX. */
vkr_internal uint32_t memory_owner_series(const VkrRecordingData *data,
                                          const char *owner,
                                          const char *suffix) {
  char name[VKR_RECORDING_NAME_MAX + 1u];
  string_format(name, sizeof(name), MEMORY_OWNER_PREFIX "%s%s", owner, suffix);
  return vkr_recording_data_find_series(data, name);
}

/* The owner's name between the prefix and the suffix of its live series. */
vkr_internal String8 memory_owner_name(const VkrRecordingData *data,
                                       uint32_t series) {
  const char *name = data->series[series].name;
  const uint64_t prefix = sizeof(MEMORY_OWNER_PREFIX) - 1u;
  const uint64_t suffix = sizeof(MEMORY_OWNER_LIVE) - 1u;
  const uint64_t length = string_length(name);
  if (length <= prefix + suffix) {
    return (String8){0};
  }
  return (String8){.str = (uint8_t *)name + prefix,
                   .length = length - prefix - suffix};
}

typedef struct MemoryRank {
  uint32_t index;
  float64_t key;
} MemoryRank;

vkr_internal int32_t memory_compare_rank(const void *lhs, const void *rhs) {
  const MemoryRank *a = lhs;
  const MemoryRank *b = rhs;
  if (a->key != b->key) {
    return a->key > b->key ? -1 : 1;
  }
  return a->index < b->index ? -1 : (a->index > b->index ? 1 : 0);
}

/* Each allocator's live bytes in `frame`, by id; false without samples. */
vkr_internal bool8_t memory_allocator_live(const VkrRecordingData *data,
                                           uint64_t frame, float64_t *live) {
  uint32_t count = 0u;
  const VkrRecordingAllocatorRow *rows =
      vkr_recording_data_allocator_rows(data, frame, &count);
  for (uint32_t i = 0u; i < data->allocator_count; ++i) {
    live[i] = 0.0;
  }
  for (uint32_t r = 0u; r < count; ++r) {
    live[rows[r].allocator] = (float64_t)rows[r].live_bytes;
  }
  return count > 0u;
}

/* Rebuilds the charts and statistics for the shown frames. */
vkr_internal void memory_update(VkrEditorRecordingPerf *perf) {
  VkrEditorPerfMemory *memory = &perf->memory_tab;
  if (memory->valid && memory->columns_version == perf->columns_version) {
    return;
  }
  const VkrRecordingData *data = &perf->data;
  MemZero(memory, sizeof(*memory));
  memory->columns_version = perf->columns_version;
  memory->valid = true_v;
  const uint32_t columns = perf->column_count;
  const uint64_t first = perf->view_first;
  const uint64_t last = Min(perf->view_last, data->frame_count - 1u);

  /* The GPU owners the recording measured. */
  const uint64_t prefix = sizeof(MEMORY_OWNER_PREFIX) - 1u;
  const uint64_t suffix = sizeof(MEMORY_OWNER_LIVE) - 1u;
  for (uint32_t i = 0u; i < data->series_count &&
                        memory->owner_count < VKR_EDITOR_PERF_GPU_OWNERS;
       ++i) {
    const char *name = data->series[i].name;
    const uint64_t length = string_length(name);
    if (length > prefix + suffix &&
        MemCompare(name, MEMORY_OWNER_PREFIX, prefix) == 0 &&
        string_equals(name + length - suffix, MEMORY_OWNER_LIVE)) {
      const uint32_t owner = memory->owner_count++;
      memory->owner_series[owner] = i;
      char owner_name[VKR_RECORDING_NAME_MAX + 1u];
      string_format(owner_name, sizeof(owner_name), "%.*s",
                    (int32_t)(length - prefix - suffix), name + prefix);
      memory->owner_allocations[owner] =
          memory_owner_series(data, owner_name, ".allocations.live");
      memory->owner_created[owner] =
          memory_owner_series(data, owner_name, ".allocations.created");
    }
  }

  /* The largest allocators and owners by their peak over the shown frames
     take a series each; the rest share the last. */
  float64_t live[VKR_RECORDING_ALLOCATOR_MAX];
  MemoryRank ranks[VKR_RECORDING_ALLOCATOR_MAX];
  for (uint32_t i = 0u; i < data->allocator_count; ++i) {
    ranks[i] = (MemoryRank){.index = i, .key = 0.0};
  }
  uint64_t sampled = 0u;
  for (uint64_t f = first; f <= last && data->allocator_count > 0u; ++f) {
    if (!memory_allocator_live(data, f, live)) {
      continue;
    }
    ++sampled;
    for (uint32_t i = 0u; i < data->allocator_count; ++i) {
      ranks[i].key = Max(ranks[i].key, live[i]);
    }
    uint32_t count = 0u;
    const VkrRecordingAllocatorRow *rows =
        vkr_recording_data_allocator_rows(data, f, &count);
    for (uint32_t r = 0u; r < count; ++r) {
      memory->allocs_per_frame += rows[r].allocs;
      memory->frees_per_frame += rows[r].frees;
      memory->scopes_per_frame += rows[r].scopes;
      memory->deepest_scope =
          Max(memory->deepest_scope, rows[r].scope_depth_high);
    }
  }
  if (sampled > 0u) {
    memory->allocs_per_frame /= (float64_t)sampled;
    memory->frees_per_frame /= (float64_t)sampled;
    memory->scopes_per_frame /= (float64_t)sampled;
    float64_t start[VKR_RECORDING_ALLOCATOR_MAX];
    float64_t end[VKR_RECORDING_ALLOCATOR_MAX];
    uint64_t a = first;
    while (a < last && !memory_allocator_live(data, a, start)) {
      ++a;
    }
    uint64_t b = last;
    while (b > a && !memory_allocator_live(data, b, end)) {
      --b;
    }
    if (b > a) {
      for (uint32_t i = 0u; i < data->allocator_count; ++i) {
        memory->live_growth += (int64_t)end[i] - (int64_t)start[i];
      }
    }
  }
  memory->ram_by_allocator = sampled > 0u;
  const uint32_t named = VKR_EDITOR_PERF_MEMORY_SERIES - 1u;
  if (memory->ram_by_allocator) {
    vkr_sort(ranks, data->allocator_count, sizeof(MemoryRank),
             memory_compare_rank);
    memory->ram_series_count = Min(data->allocator_count, named);
    for (uint32_t i = 0u; i < memory->ram_series_count; ++i) {
      memory->ram_series[i] = ranks[i].index;
    }
  }
  MemoryRank owners[VKR_EDITOR_PERF_GPU_OWNERS];
  for (uint32_t o = 0u; o < memory->owner_count; ++o) {
    owners[o] = (MemoryRank){.index = o, .key = 0.0};
    for (uint64_t f = first; f <= last; ++f) {
      float64_t bytes = 0.0;
      if (memory_value(data, memory->owner_series[o], f, &bytes)) {
        owners[o].key = Max(owners[o].key, bytes);
      }
    }
  }
  vkr_sort(owners, memory->owner_count, sizeof(MemoryRank),
           memory_compare_rank);
  memory->vram_series_count = Min(memory->owner_count, named);
  for (uint32_t i = 0u; i < memory->vram_series_count; ++i) {
    memory->vram_series[i] = owners[i].index;
  }

  /* Each column at its frame: stacked megabytes, and its allocations and
     frees. */
  const uint32_t resident =
      vkr_recording_data_find_series(data, "process.resident_bytes");
  for (uint32_t c = 0u; c < columns; ++c) {
    const uint64_t f = perf->column_frame[c];
    float32_t ram_total = 0.0f;
    if (memory->ram_by_allocator && memory_allocator_live(data, f, live)) {
      float64_t rest = 0.0;
      for (uint32_t i = 0u; i < data->allocator_count; ++i) {
        rest += live[i];
      }
      for (uint32_t s = 0u; s < memory->ram_series_count; ++s) {
        const float64_t bytes = live[memory->ram_series[s]];
        memory->ram_mb[s][c] = (float32_t)(bytes / MEMORY_MB);
        rest -= bytes;
      }
      memory->ram_mb[named][c] = (float32_t)(Max(rest, 0.0) / MEMORY_MB);
      for (uint32_t s = 0u; s < VKR_EDITOR_PERF_MEMORY_SERIES; ++s) {
        ram_total += memory->ram_mb[s][c];
      }
    } else if (!memory->ram_by_allocator) {
      float64_t bytes = 0.0;
      if (memory_value(data, resident, f, &bytes)) {
        memory->ram_mb[0][c] = (float32_t)(bytes / MEMORY_MB);
        ram_total = memory->ram_mb[0][c];
      }
    }
    memory->ram_top = Max(memory->ram_top, ram_total);
    float32_t vram_total = 0.0f;
    float64_t vram_rest = 0.0;
    for (uint32_t o = 0u; o < memory->owner_count; ++o) {
      float64_t bytes = 0.0;
      if (memory_value(data, memory->owner_series[o], f, &bytes)) {
        vram_rest += bytes;
      }
    }
    for (uint32_t s = 0u; s < memory->vram_series_count; ++s) {
      float64_t bytes = 0.0;
      (void)memory_value(data, memory->owner_series[memory->vram_series[s]], f,
                         &bytes);
      memory->vram_mb[s][c] = (float32_t)(bytes / MEMORY_MB);
      vram_rest -= bytes;
      vram_total += memory->vram_mb[s][c];
    }
    memory->vram_mb[named][c] = (float32_t)(Max(vram_rest, 0.0) / MEMORY_MB);
    vram_total += memory->vram_mb[named][c];
    memory->vram_top = Max(memory->vram_top, vram_total);
    uint32_t count = 0u;
    const VkrRecordingAllocatorRow *rows =
        vkr_recording_data_allocator_rows(data, f, &count);
    for (uint32_t r = 0u; r < count; ++r) {
      memory->allocs[c] += (float32_t)rows[r].allocs;
      memory->frees[c] -= (float32_t)rows[r].frees;
    }
    memory->rate_top =
        Max(memory->rate_top, Max(memory->allocs[c], -memory->frees[c]));
  }
  memory->ram_top = Max(memory->ram_top * 1.08f, 1.0f);
  memory->vram_top = Max(memory->vram_top * 1.08f, 1.0f);
  memory->rate_top = Max(memory->rate_top * 1.08f, 1.0f);
}

/* ------------------------------------------------------------------------ */
/* Cards                                                                     */

/* Mean and peak of series `name` over the shown frames, in GiB; false
   without samples. */
vkr_internal bool8_t memory_series_view(const VkrEditorRecordingPerf *perf,
                                        const char *name, float64_t *mean,
                                        float64_t *peak) {
  const VkrRecordingData *data = &perf->data;
  const uint32_t series = vkr_recording_data_find_series(data, name);
  uint64_t count = 0u;
  *mean = 0.0;
  *peak = 0.0;
  const uint64_t last = Min(perf->view_last, data->frame_count - 1u);
  for (uint64_t f = perf->view_first; f <= last; ++f) {
    float64_t value = 0.0;
    if (memory_value(data, series, f, &value)) {
      *mean += value;
      *peak = Max(*peak, value);
      ++count;
    }
  }
  if (count == 0u) {
    return false_v;
  }
  *mean /= (float64_t)count;
  return true_v;
}

vkr_internal String8 memory_gib_pair(VkrUiSystem *ui,
                                     const VkrEditorRecordingPerf *perf,
                                     const char *name, const char *fallback) {
  float64_t mean = 0.0;
  float64_t peak = 0.0;
  if (!memory_series_view(perf, name, &mean, &peak) &&
      (!fallback || !memory_series_view(perf, fallback, &mean, &peak))) {
    return string8_lit("not recorded");
  }
  return string8_create_formatted(ui->frame_allocator, "%.2f / %.2f GiB",
                                  mean / (MEMORY_MB * 1024.0),
                                  peak / (MEMORY_MB * 1024.0));
}

vkr_internal void memory_cards(VkrEditorUi *editor, VkrUiSystem *ui,
                               VkrUiPlacement placement) {
  const VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrEditorPerfMemory *memory = &perf->memory_tab;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack card = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack wide = {.value = 1.4f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack columns[] = {wide, card, card, card, card, wide, wide};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig row =
      vkr_editor_perf_grid(placement, columns, ArrayCount(columns), &fill, 1u);
  row.style.gap_pt = theme->space_sm;
  if (!vkr_ui_panel_begin(ui, string8_lit("memory.cards"), &row)) {
    return;
  }
  const bool8_t named = memory->ram_by_allocator;
  const String8 none = string8_lit("no named allocators");
  vkr_editor_perf_card(editor, ui, 0u, "RAM mean / peak",
                       memory_gib_pair(ui, perf, "os.memory.footprint_bytes",
                                       "process.resident_bytes"),
                       theme->text, string8_lit("process footprint"),
                       theme->text_secondary);
  vkr_editor_perf_card(
      editor, ui, 1u, "Allocations",
      named ? string8_create_formatted(ui->frame_allocator, "%.0f",
                                       memory->allocs_per_frame)
            : string8_lit("n/a"),
      theme->text, named ? string8_lit("per frame") : none,
      theme->text_secondary);
  vkr_editor_perf_card(editor, ui, 2u, "Frees",
                       named ? string8_create_formatted(ui->frame_allocator,
                                                        "%.0f",
                                                        memory->frees_per_frame)
                             : string8_lit("n/a"),
                       theme->text, named ? string8_lit("per frame") : none,
                       theme->text_secondary);
  /* Growth in the named allocators over the shown frames: a leak shows as
     a steady climb. */
  const float64_t growth_mb = (float64_t)memory->live_growth / MEMORY_MB;
  vkr_editor_perf_card(editor, ui, 3u, "Live growth",
                       named ? string8_create_formatted(ui->frame_allocator,
                                                        "%+.1f MB", growth_mb)
                             : string8_lit("n/a"),
                       named && growth_mb > 16.0 ? theme->warning : theme->text,
                       named ? string8_lit("over the shown frames") : none,
                       theme->text_secondary);
  vkr_editor_perf_card(
      editor, ui, 4u, "Scopes",
      named ? string8_create_formatted(ui->frame_allocator, "%.0f",
                                       memory->scopes_per_frame)
            : string8_lit("n/a"),
      theme->text,
      named ? string8_create_formatted(ui->frame_allocator,
                                       "per frame  \xc2\xb7  deepest %u",
                                       memory->deepest_scope)
            : none,
      theme->text_secondary);
  vkr_editor_perf_card(editor, ui, 5u, "VRAM mean / peak",
                       memory_gib_pair(ui, perf, "memory.gpu.bytes.live",
                                       "memory.gpu.driver."
                                       "current_allocated_bytes"),
                       theme->text, string8_lit("live GPU allocations"),
                       theme->text_secondary);
  float64_t used = 0.0;
  float64_t peak = 0.0;
  float64_t budget = 0.0;
  float64_t budget_peak = 0.0;
  const bool8_t driver =
      memory_series_view(perf, "memory.gpu.driver.current_allocated_bytes",
                         &used, &peak) &&
      memory_series_view(perf, "memory.gpu.driver.recommended_budget_bytes",
                         &budget, &budget_peak) &&
      budget > 0.0;
  vkr_editor_perf_card(
      editor, ui, 6u, "Driver budget",
      driver ? string8_create_formatted(ui->frame_allocator, "%.2f GiB",
                                        budget / (MEMORY_MB * 1024.0))
             : string8_lit("not recorded"),
      theme->text,
      driver ? string8_create_formatted(ui->frame_allocator, "peak use %.0f%%",
                                        100.0 * peak / budget)
             : (String8){0},
      driver && peak > 0.9 * budget ? theme->warning : theme->text_secondary);
  (void)vkr_ui_panel_end(ui);
}

/* ------------------------------------------------------------------------ */
/* Charts                                                                    */

/* The tooltip of a stacked memory chart's hovered column. */
vkr_internal String8 memory_tooltip(VkrUiSystem *ui,
                                    const VkrEditorRecordingPerf *perf,
                                    bool8_t vram) {
  const VkrEditorPerfMemory *memory = &perf->memory_tab;
  const uint32_t c = perf->hovered_column;
  if (c >= perf->column_count) {
    return (String8){0};
  }
  const VkrRecordingData *data = &perf->data;
  String8 text = string8_create_formatted(
      ui->frame_allocator, "Frame %llu  \xc2\xb7  %.3f s",
      (unsigned long long)perf->column_frame[c],
      data->times[perf->column_frame[c]]);
  const uint32_t count = vram                       ? memory->vram_series_count
                         : memory->ram_by_allocator ? memory->ram_series_count
                                                    : 1u;
  for (uint32_t s = 0u; s < count; ++s) {
    String8 name = string8_lit("Resident");
    if (vram) {
      name =
          memory_owner_name(data, memory->owner_series[memory->vram_series[s]]);
    } else if (memory->ram_by_allocator) {
      const char *allocator = data->allocators[memory->ram_series[s]].name;
      name = string8_create_from_cstr((const uint8_t *)allocator,
                                      string_length(allocator));
    }
    text = string8_create_formatted(
        ui->frame_allocator, "%.*s\n%.*s  %.1f MB", (int32_t)text.length,
        text.str, (int32_t)name.length, (const char *)name.str,
        vram ? memory->vram_mb[s][c] : memory->ram_mb[s][c]);
  }
  const uint32_t rest = VKR_EDITOR_PERF_MEMORY_SERIES - 1u;
  const float32_t other =
      vram ? memory->vram_mb[rest][c] : memory->ram_mb[rest][c];
  if (other > 0.0f) {
    text = string8_create_formatted(ui->frame_allocator, "%.*s\nOther  %.1f MB",
                                    (int32_t)text.length, text.str, other);
  }
  return text;
}

/* A stacked chart of RAM by allocator or VRAM by owner, under its title
   and key. */
vkr_internal void memory_stack(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame,
                               VkrUiPlacement placement, bool8_t vram) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrEditorPerfMemory *memory = &perf->memory_tab;
  const VkrRecordingData *data = &perf->data;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.value = VKR_EDITOR_PERF_CHART_TITLE_PT, .unit = VKR_UI_TRACK_PX}, fill};
  VkrUiPanelConfig panel =
      vkr_editor_perf_grid(placement, &fill, 1u, rows, ArrayCount(rows));
  panel.style.gap_pt = vkr_ui_theme()->space_xs;
  if (!vkr_ui_panel_begin(
          ui, vram ? string8_lit("memory.vram") : string8_lit("memory.ram"),
          &panel)) {
    return;
  }
  const uint32_t named = vram                       ? memory->vram_series_count
                         : memory->ram_by_allocator ? memory->ram_series_count
                                                    : 1u;
  const char *keys[VKR_EDITOR_PERF_MEMORY_SERIES];
  Vec4 colors[VKR_EDITOR_PERF_MEMORY_SERIES];
  uint32_t key_count = 0u;
  for (uint32_t s = 0u; s < named; ++s) {
    if (vram) {
      const String8 owner =
          memory_owner_name(data, memory->owner_series[memory->vram_series[s]]);
      keys[key_count] = (const char *)string8_create_formatted(
                            ui->frame_allocator, "%.*s", (int32_t)owner.length,
                            (const char *)owner.str)
                            .str;
    } else {
      keys[key_count] = memory->ram_by_allocator
                            ? data->allocators[memory->ram_series[s]].name
                            : "Resident";
    }
    colors[key_count++] = MEMORY_COLOR[s];
  }
  keys[key_count] = "Other";
  colors[key_count++] = MEMORY_COLOR[VKR_EDITOR_PERF_MEMORY_SERIES - 1u];
  const float32_t top = vram ? memory->vram_top : memory->ram_top;
  vkr_editor_perf_chart_title(
      ui, "title", vkr_editor_perf_cell(0u, 0u),
      string8_create_formatted(ui->frame_allocator,
                               "%s (MB), stacked  \xc2\xb7  top %.0f MB",
                               vram                       ? "VRAM by GPU owner"
                               : memory->ram_by_allocator ? "RAM by allocator"
                                                          : "RAM, resident",
                               top),
      keys, colors, key_count);
  VkrUiPlotSeries bars[VKR_EDITOR_PERF_MEMORY_SERIES];
  uint32_t bar_count = 0u;
  for (uint32_t s = 0u; s < named; ++s) {
    bars[bar_count++] = (VkrUiPlotSeries){.values = vram ? memory->vram_mb[s]
                                                         : memory->ram_mb[s],
                                          .color = MEMORY_COLOR[s]};
  }
  const uint32_t rest = VKR_EDITOR_PERF_MEMORY_SERIES - 1u;
  bars[bar_count++] = (VkrUiPlotSeries){.values = vram ? memory->vram_mb[rest]
                                                       : memory->ram_mb[rest],
                                        .color = MEMORY_COLOR[rest]};
  VkrUiPlotConfig plot = {
      .sample_count = perf->column_count,
      .y_min = 0.0f,
      .y_max = top,
      .bars = bars,
      .bar_count = bar_count,
  };
  vkr_editor_perf_plot(editor, frame, vram ? "vram.plot" : "ram.plot",
                       vkr_editor_perf_cell(0u, 1u), &plot, false_v,
                       memory_tooltip(ui, perf, vram));
  (void)vkr_ui_panel_end(ui);
}

vkr_internal void memory_rate(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame,
                              VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrEditorPerfMemory *memory = &perf->memory_tab;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.value = VKR_EDITOR_PERF_CHART_TITLE_PT, .unit = VKR_UI_TRACK_PX}, fill};
  VkrUiPanelConfig panel =
      vkr_editor_perf_grid(placement, &fill, 1u, rows, ArrayCount(rows));
  panel.style.gap_pt = theme->space_xs;
  if (!vkr_ui_panel_begin(ui, string8_lit("memory.rate"), &panel)) {
    return;
  }
  static const char *const keys[] = {"Allocations", "Frees"};
  const Vec4 colors[] = {MEMORY_ALLOC_COLOR, MEMORY_FREE_COLOR};
  vkr_editor_perf_chart_title(
      ui, "title", vkr_editor_perf_cell(0u, 0u),
      string8_create_formatted(ui->frame_allocator,
                               "Allocations above the line, frees below, per "
                               "frame  \xc2\xb7  named allocators  \xc2\xb7  "
                               "top %.0f",
                               memory->rate_top),
      keys, colors, ArrayCount(keys));
  const VkrUiPlotSeries bars[] = {
      {.values = memory->allocs, .color = MEMORY_ALLOC_COLOR},
      {.values = memory->frees, .color = MEMORY_FREE_COLOR},
  };
  const VkrUiPlotReference zero = {
      .value = 0.0f, .color = vkr_ui_color_alpha(theme->text_secondary, 0.6f)};
  String8 tip = (String8){0};
  const uint32_t c = perf->hovered_column;
  if (c < perf->column_count) {
    tip = string8_create_formatted(
        ui->frame_allocator,
        "Frame %llu\nAllocations %.0f  \xc2\xb7  frees %.0f  \xc2\xb7  net "
        "%+.0f",
        (unsigned long long)perf->column_frame[c], memory->allocs[c],
        -memory->frees[c], memory->allocs[c] + memory->frees[c]);
  }
  VkrUiPlotConfig plot = {
      .sample_count = perf->column_count,
      .y_min = -memory->rate_top,
      .y_max = memory->rate_top,
      .bars = bars,
      .bar_count = ArrayCount(bars),
      .references = &zero,
      .reference_count = 1u,
  };
  vkr_editor_perf_plot(editor, frame, "rate.plot", vkr_editor_perf_cell(0u, 1u),
                       &plot, false_v, tip);
  (void)vkr_ui_panel_end(ui);
}

/* ------------------------------------------------------------------------ */
/* Tables                                                                    */

enum {
  WHERE_POOL = 0,
  WHERE_NAME,
  WHERE_LIVE,
  WHERE_SHARE,
  WHERE_GROWTH,
  WHERE_PEAK,
  WHERE_ALLOCS,
  WHERE_FREES,
  WHERE_COUNT,
};

static const VkrEditorTableColumn WHERE_COLUMNS[WHERE_COUNT] = {
    {.title = "Pool",
     .width_pt = 42.0f,
     .min_pt = 36.0f,
     .tooltip = "RAM from a named allocator, or VRAM of a GPU owner"},
    {.title = "Allocator or owner", .width_pt = 0.0f, .min_pt = 120.0f},
    {.title = "Live", .width_pt = 70.0f, .min_pt = 56.0f, .numeric = true_v},
    {.title = "Share",
     .width_pt = 68.0f,
     .min_pt = 50.0f,
     .numeric = true_v,
     .tooltip = "Its part of its pool's live bytes"},
    {.title = "Growth",
     .width_pt = 70.0f,
     .min_pt = 56.0f,
     .numeric = true_v,
     .tooltip = "Live bytes since the first shown frame"},
    {.title = "Peak",
     .width_pt = 70.0f,
     .min_pt = 56.0f,
     .numeric = true_v,
     .tooltip = "Highest live bytes over the shown frames"},
    {.title = "Allocs",
     .width_pt = 52.0f,
     .min_pt = 44.0f,
     .numeric = true_v,
     .tooltip = "Allocations in this frame; for VRAM, created this frame"},
    {.title = "Frees",
     .width_pt = 50.0f,
     .min_pt = 44.0f,
     .numeric = true_v,
     .tooltip = "Frees in this frame"},
};

typedef struct MemoryWhere {
  bool8_t vram;
  uint32_t index;
  String8 name;
  float64_t live;
  float64_t start;
  float64_t peak;
  float64_t allocs;
  float64_t frees;
} MemoryWhere;

vkr_internal String8 memory_bytes(VkrUiSystem *ui, float64_t bytes) {
  return vkr_abs_f64(bytes) >= 1024.0 * MEMORY_MB
             ? string8_create_formatted(ui->frame_allocator, "%.2f GB",
                                        bytes / (1024.0 * MEMORY_MB))
             : string8_create_formatted(ui->frame_allocator, "%.1f MB",
                                        bytes / MEMORY_MB);
}

/* Every allocator and owner at `at`, with its growth and peak over the
   shown frames. */
vkr_internal uint32_t memory_where_rows(const VkrEditorRecordingPerf *perf,
                                        uint64_t at, MemoryWhere *out) {
  const VkrRecordingData *data = &perf->data;
  const VkrEditorPerfMemory *memory = &perf->memory_tab;
  const uint64_t first = perf->view_first;
  const uint64_t last = Min(perf->view_last, data->frame_count - 1u);
  uint32_t count = 0u;
  if (memory->ram_by_allocator) {
    float64_t live[VKR_RECORDING_ALLOCATOR_MAX];
    float64_t start[VKR_RECORDING_ALLOCATOR_MAX];
    float64_t peak[VKR_RECORDING_ALLOCATOR_MAX];
    (void)memory_allocator_live(data, at, live);
    uint64_t a = first;
    while (a < last && !memory_allocator_live(data, a, start)) {
      ++a;
    }
    for (uint32_t i = 0u; i < data->allocator_count; ++i) {
      peak[i] = 0.0;
    }
    float64_t sample[VKR_RECORDING_ALLOCATOR_MAX];
    for (uint64_t f = first; f <= last; ++f) {
      if (memory_allocator_live(data, f, sample)) {
        for (uint32_t i = 0u; i < data->allocator_count; ++i) {
          peak[i] = Max(peak[i], sample[i]);
        }
      }
    }
    uint32_t row_count = 0u;
    const VkrRecordingAllocatorRow *rows =
        vkr_recording_data_allocator_rows(data, at, &row_count);
    for (uint32_t i = 0u; i < data->allocator_count; ++i) {
      if (peak[i] <= 0.0 && live[i] <= 0.0) {
        continue;
      }
      MemoryWhere *where = &out[count++];
      *where = (MemoryWhere){
          .vram = false_v,
          .index = i,
          .name = string8_create_from_cstr(
              (const uint8_t *)data->allocators[i].name,
              string_length(data->allocators[i].name)),
          .live = live[i],
          .start = start[i],
          .peak = peak[i],
      };
      for (uint32_t r = 0u; r < row_count; ++r) {
        if (rows[r].allocator == i) {
          where->allocs = rows[r].allocs;
          where->frees = rows[r].frees;
        }
      }
    }
  }
  for (uint32_t o = 0u; o < memory->owner_count; ++o) {
    const uint32_t series = memory->owner_series[o];
    MemoryWhere where = {
        .vram = true_v,
        .index = o,
        .name = memory_owner_name(data, series),
    };
    (void)memory_value(data, series, at, &where.live);
    uint64_t a = first;
    while (a < last && !memory_value(data, series, a, &where.start)) {
      ++a;
    }
    for (uint64_t f = first; f <= last; ++f) {
      float64_t bytes = 0.0;
      if (memory_value(data, series, f, &bytes)) {
        where.peak = Max(where.peak, bytes);
      }
    }
    (void)memory_value(data, memory->owner_created[o], at, &where.allocs);
    if (where.peak > 0.0 || where.live > 0.0) {
      out[count++] = where;
    }
  }
  return count;
}

vkr_internal float64_t memory_where_key(const MemoryWhere *where,
                                        uint32_t column) {
  switch (column) {
  case WHERE_POOL:
    return where->vram ? 1.0 : 0.0;
  case WHERE_GROWTH:
    return where->live - where->start;
  case WHERE_PEAK:
    return where->peak;
  case WHERE_ALLOCS:
    return where->allocs;
  case WHERE_FREES:
    return where->frees;
  default:
    return where->live;
  }
}

typedef struct MemoryOrder {
  uint32_t row;
  float64_t key;
} MemoryOrder;

vkr_internal int32_t memory_compare_order(const void *lhs, const void *rhs) {
  const MemoryOrder *a = lhs;
  const MemoryOrder *b = rhs;
  if (a->key != b->key) {
    return a->key > b->key ? -1 : 1;
  }
  return a->row < b->row ? -1 : (a->row > b->row ? 1 : 0);
}

vkr_internal void memory_where(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame,
                               VkrUiPlacement placement, uint64_t at) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrEditorTable *table = &perf->view.memory_where;
  if (!table->initialized) {
    table->sort_column = WHERE_LIVE;
    table->sort_descending = true_v;
  }
  MemoryWhere *rows = vkr_allocator_alloc(
      ui->frame_allocator, sizeof(MemoryWhere) * MEMORY_WHERE_MAX,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  MemoryOrder *order = vkr_allocator_alloc(
      ui->frame_allocator, sizeof(MemoryOrder) * MEMORY_WHERE_MAX,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!rows || !order) {
    return;
  }
  const uint32_t count = memory_where_rows(perf, at, rows);
  float64_t pool_total[2] = {0.0, 0.0};
  float64_t pool_largest[2] = {1.0, 1.0};
  for (uint32_t i = 0u; i < count; ++i) {
    pool_total[rows[i].vram ? 1 : 0] += rows[i].live;
    pool_largest[rows[i].vram ? 1 : 0] =
        Max(pool_largest[rows[i].vram ? 1 : 0], rows[i].live);
    const uint32_t column = table->sort_column < WHERE_COUNT
                                ? table->sort_column
                                : (uint32_t)WHERE_LIVE;
    order[i] =
        (MemoryOrder){.row = i, .key = memory_where_key(&rows[i], column)};
  }
  vkr_sort(order, count, sizeof(MemoryOrder), memory_compare_order);
  const bool8_t ascending =
      table->sort_column < WHERE_COUNT && !table->sort_descending;
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, "memory.where", table, WHERE_COLUMNS,
                              WHERE_COUNT, count, placement, &sorted, &first,
                              &end)) {
    return;
  }
  for (uint32_t i = first; i < end; ++i) {
    const MemoryWhere *where = &rows[order[ascending ? count - 1u - i : i].row];
    const float64_t growth = where->live - where->start;
    const float64_t total = Max(pool_total[where->vram ? 1 : 0], 1.0);
    String8 cells[WHERE_COUNT] = {
        where->vram ? string8_lit("VRAM") : string8_lit("RAM"),
        where->name,
        memory_bytes(ui, where->live),
        string8_create_formatted(ui->frame_allocator, "%.1f%%",
                                 100.0 * where->live / total),
        string8_create_formatted(ui->frame_allocator, "%+.1f MB",
                                 growth / MEMORY_MB),
        memory_bytes(ui, where->peak),
        string8_create_formatted(ui->frame_allocator, "%.0f", where->allocs),
        where->vram ? (String8){0}
                    : string8_create_formatted(ui->frame_allocator, "%.0f",
                                               where->frees),
    };
    Vec4 colors[WHERE_COUNT] = {0};
    colors[WHERE_POOL] = theme->text_secondary;
    colors[WHERE_GROWTH] = growth > MEMORY_MB    ? theme->warning
                           : growth < -MEMORY_MB ? theme->success
                                                 : theme->text_disabled;
    float32_t bars[WHERE_COUNT];
    Vec4 bar_colors[WHERE_COUNT] = {0};
    for (uint32_t c = 0u; c < WHERE_COUNT; ++c) {
      bars[c] = -1.0f;
    }
    /* The bar measures against its pool's largest, so small ones still
       show. */
    bars[WHERE_SHARE] =
        (float32_t)(where->live / pool_largest[where->vram ? 1 : 0]);
    bar_colors[WHERE_SHARE] = where->vram ? MEMORY_COLOR[1] : MEMORY_COLOR[0];
    const VkrEditorTableRow row = {
        .key = (where->vram ? 0x10000u : 0u) + where->index,
        .cells = cells,
        .cell_colors = colors,
        .cell_bars = bars,
        .bar_colors = bar_colors,
    };
    (void)vkr_editor_table_row(ui, table, WHERE_COLUMNS, WHERE_COUNT, i, &row);
  }
  vkr_editor_table_end(ui);
}

enum {
  ARENA_NAME = 0,
  ARENA_USED,
  ARENA_FOOTPRINT,
  ARENA_PEAK,
  ARENA_SCOPES,
  ARENA_DEPTH,
  ARENA_COUNT,
};

static const VkrEditorTableColumn ARENA_COLUMNS[ARENA_COUNT] = {
    {.title = "Arena", .width_pt = 0.0f, .min_pt = 110.0f},
    {.title = "Used",
     .width_pt = 132.0f,
     .min_pt = 90.0f,
     .numeric = true_v,
     .tooltip = "Used of reserved; the bar is the used share of the "
                "reservation"},
    {.title = "Committed / reserved",
     .width_pt = 128.0f,
     .min_pt = 90.0f,
     .numeric = true_v},
    {.title = "Peak",
     .width_pt = 70.0f,
     .min_pt = 56.0f,
     .numeric = true_v,
     .tooltip = "Highest used size during this frame"},
    {.title = "Scopes",
     .width_pt = 58.0f,
     .min_pt = 46.0f,
     .numeric = true_v,
     .tooltip = "Scopes begun in this frame"},
    {.title = "Depth",
     .width_pt = 50.0f,
     .min_pt = 40.0f,
     .numeric = true_v,
     .tooltip = "Deepest scope nesting in this frame"},
};

vkr_internal void memory_arenas(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame,
                                VkrUiPlacement placement, uint64_t at) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  const VkrRecordingData *data = &perf->data;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrEditorTable *table = &perf->view.memory_arenas;
  uint32_t count = 0u;
  const VkrRecordingAllocatorRow *rows =
      vkr_recording_data_allocator_rows(data, at, &count);
  uint32_t arenas[VKR_RECORDING_ALLOCATOR_MAX];
  uint32_t arena_count = 0u;
  for (uint32_t r = 0u; r < count; ++r) {
    if (rows[r].reserved_bytes > 0u) {
      arenas[arena_count++] = r;
    }
  }
  if (arena_count == 0u) {
    vkr_editor_perf_empty(
        editor, ui, placement, VKR_UI_ICON_MEMORY, "No arenas at this frame",
        data->allocator_count > 0u
            ? "The named allocators of this frame are not arena-backed."
            : "Tier 1 recordings from this build sample the engine's named "
              "allocators and arenas every frame.");
    return;
  }
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, "memory.arenas", table, ARENA_COLUMNS,
                              ARENA_COUNT, arena_count, placement, &sorted,
                              &first, &end)) {
    return;
  }
  for (uint32_t i = first; i < end; ++i) {
    const VkrRecordingAllocatorRow *row = &rows[arenas[i]];
    const char *name = data->allocators[row->allocator].name;
    String8 cells[ARENA_COUNT] = {
        string8_create_from_cstr((const uint8_t *)name, string_length(name)),
        memory_bytes(ui, (float64_t)row->used_bytes),
        string8_create_formatted(
            ui->frame_allocator, "%.*s / %.*s",
            (int32_t)memory_bytes(ui, (float64_t)row->committed_bytes).length,
            (const char *)memory_bytes(ui, (float64_t)row->committed_bytes).str,
            (int32_t)memory_bytes(ui, (float64_t)row->reserved_bytes).length,
            (const char *)memory_bytes(ui, (float64_t)row->reserved_bytes).str),
        memory_bytes(ui, (float64_t)row->peak_used_bytes),
        string8_create_formatted(ui->frame_allocator, "%u", row->scopes),
        string8_create_formatted(ui->frame_allocator, "%u",
                                 row->scope_depth_high),
    };
    float32_t bars[ARENA_COUNT];
    Vec4 bar_colors[ARENA_COUNT] = {0};
    for (uint32_t c = 0u; c < ARENA_COUNT; ++c) {
      bars[c] = -1.0f;
    }
    const float32_t share =
        (float32_t)row->used_bytes / (float32_t)row->reserved_bytes;
    bars[ARENA_USED] = share;
    bar_colors[ARENA_USED] = share > 0.85f ? theme->warning : MEMORY_COLOR[0];
    const VkrEditorTableRow table_row = {
        .key = row->allocator,
        .cells = cells,
        .cell_bars = bars,
        .bar_colors = bar_colors,
    };
    (void)vkr_editor_table_row(ui, table, ARENA_COLUMNS, ARENA_COUNT, i,
                               &table_row);
  }
  vkr_editor_table_end(ui);
}

/* ------------------------------------------------------------------------ */
/* Tab                                                                       */

void vkr_editor_recording_perf_memory_build(VkrEditorUi *editor,
                                            const VkrSampleUiFrame *frame,
                                            VkrUiPlacement placement) {
  VkrEditorRecordingPerf *perf = &editor->recording.perf;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  memory_update(perf);
  const VkrEditorPerfMemory *memory = &perf->memory_tab;
  if (!memory->ram_by_allocator && memory->owner_count == 0u &&
      vkr_recording_data_find_series(&perf->data, "process.resident_bytes") ==
          UINT32_MAX) {
    vkr_editor_perf_empty(
        editor, ui, placement, VKR_UI_ICON_MEMORY, "No memory recorded",
        "Tier 0 recordings hold the process's memory; Tier 1 recordings also "
        "hold the named allocators and arenas and VRAM by GPU owner.");
    return;
  }
  const uint64_t focus = vkr_editor_perf_focus(perf, frame);
  const uint64_t at = focus < perf->data.frame_count ? focus : perf->view_last;
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack title = {.value = 18.0f, .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack rows[] = {
      {.value = 62.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 1.3f, .unit = VKR_UI_TRACK_FR},
      {.value = 0.8f, .unit = VKR_UI_TRACK_FR},
      title,
      {.value = 1.4f, .unit = VKR_UI_TRACK_FR},
  };
  VkrUiPanelConfig body =
      vkr_editor_perf_grid(placement, &fill, 1u, rows, ArrayCount(rows));
  body.style.gap_pt = theme->space_sm;
  if (!vkr_ui_panel_begin(ui, string8_lit("memory.body"), &body)) {
    return;
  }
  memory_cards(editor, ui, vkr_editor_perf_cell(0u, 0u));
  const VkrUiTrack halves[] = {fill, fill};
  VkrUiPanelConfig charts = vkr_editor_perf_grid(
      vkr_editor_perf_cell(0u, 1u), halves, ArrayCount(halves), &fill, 1u);
  charts.style.gap_pt = theme->space_md;
  if (vkr_ui_panel_begin(ui, string8_lit("memory.charts"), &charts)) {
    memory_stack(editor, frame, vkr_editor_perf_cell(0u, 0u), false_v);
    memory_stack(editor, frame, vkr_editor_perf_cell(1u, 0u), true_v);
    (void)vkr_ui_panel_end(ui);
  }
  memory_rate(editor, frame, vkr_editor_perf_cell(0u, 2u));
  VkrUiPanelConfig titles = vkr_editor_perf_grid(
      vkr_editor_perf_cell(0u, 3u), halves, ArrayCount(halves), &fill, 1u);
  titles.style.gap_pt = theme->space_md;
  if (vkr_ui_panel_begin(ui, string8_lit("memory.titles"), &titles)) {
    vkr_editor_perf_caption(
        ui, "where",
        string8_create_formatted(ui->frame_allocator,
                                 "Where it lives at frame %llu  \xc2\xb7  RAM "
                                 "and VRAM listed apart, never summed",
                                 (unsigned long long)at),
        vkr_editor_perf_cell(0u, 0u), theme->text_secondary);
    vkr_editor_perf_caption(
        ui, "arenas",
        string8_create_formatted(ui->frame_allocator,
                                 "Arenas and scopes at frame %llu",
                                 (unsigned long long)at),
        vkr_editor_perf_cell(1u, 0u), theme->text_secondary);
    (void)vkr_ui_panel_end(ui);
  }
  VkrUiPanelConfig tables = vkr_editor_perf_grid(
      vkr_editor_perf_cell(0u, 4u), halves, ArrayCount(halves), &fill, 1u);
  tables.style.gap_pt = theme->space_md;
  if (vkr_ui_panel_begin(ui, string8_lit("memory.tables"), &tables)) {
    memory_where(editor, frame, vkr_editor_perf_cell(0u, 0u), at);
    memory_arenas(editor, frame, vkr_editor_perf_cell(1u, 0u), at);
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_panel_end(ui);
}
