#include "editor_recording.h"

#include "editor_internal.h"
#include "editor_ui.h"
#include "platform/vkr_platform.h"

/* The timeline (docs/proposals/recordings.md, "Player"): a panel over the
   playback bar with the frame time of the frames it shows, the markers,
   notes and segments on them, a ruler, zoom, and actions on frames brushed
   across the frame-time lane. The bar's scrub still covers the whole
   recording; the timeline looks closer. */

#define EDITOR_TIMELINE_HEIGHT_PT 150.0f
/* The memory lane's height, and the gap between the panel's rows. */
#define EDITOR_TIMELINE_MEMORY_PT 34.0f
#define EDITOR_TIMELINE_ROW_GAP_PT 3.0f
#define EDITOR_TIMELINE_GAP_PT 6.0f
/* The view shows no fewer frames than this. */
#define EDITOR_TIMELINE_MIN_FRAMES 8u
/* One wheel notch zooms by this factor around the pointer. */
#define EDITOR_TIMELINE_ZOOM 0.8f
/* A ruler label wants at least this much room. */
#define EDITOR_TIMELINE_TICK_PT 90.0f
#define EDITOR_TIMELINE_MARK_PT 9.0f
/* The engine-event lane's height and its ticks' width. */
#define EDITOR_TIMELINE_ENGINE_PT 7.0f
#define EDITOR_TIMELINE_TICK_WIDTH_PT 4.0f
/* The hover preview's image width. */
#define EDITOR_TIMELINE_PREVIEW_PT 192.0f
/* The System lane: its legend line, then one band per platform signal and
   a row of error ticks, each band this tall with this gap, and the room
   under the last. */
#define EDITOR_TIMELINE_SYSTEM_TOP_PT 16.0f
#define EDITOR_TIMELINE_SYSTEM_BAND_PT 4.0f
#define EDITOR_TIMELINE_SYSTEM_BAND_GAP_PT 2.0f
#define EDITOR_TIMELINE_SYSTEM_BOTTOM_PT 2.0f

/* The System lane's rows: the three signal bands, and the error ticks when
   the recording counts errors. */
vkr_internal uint32_t
editor_timeline_system_rows(const VkrEditorRecordingTimeline *timeline) {
  return timeline->system_errors ? VKR_EDITOR_TIMELINE_SYSTEM_ROWS
                                 : VKR_EDITOR_TIMELINE_SYSTEM_ROWS - 1u;
}

/* The System lane's height: as tall as the memory lane with three bands. */
vkr_internal float32_t
editor_timeline_system_height(const VkrEditorRecordingTimeline *timeline) {
  const float32_t rows = (float32_t)editor_timeline_system_rows(timeline);
  return EDITOR_TIMELINE_SYSTEM_TOP_PT + rows * EDITOR_TIMELINE_SYSTEM_BAND_PT +
         (rows - 1.0f) * EDITOR_TIMELINE_SYSTEM_BAND_GAP_PT +
         EDITOR_TIMELINE_SYSTEM_BOTTOM_PT;
}

/* The panel's height: taller by the memory and System lanes when they
   show. */
vkr_internal float32_t
editor_timeline_panel_height(const VkrEditorRecordingTimeline *timeline) {
  float32_t height = EDITOR_TIMELINE_HEIGHT_PT;
  if (timeline->memory_lane) {
    height += EDITOR_TIMELINE_MEMORY_PT + EDITOR_TIMELINE_ROW_GAP_PT;
  }
  if (timeline->system_lane) {
    height +=
        editor_timeline_system_height(timeline) + EDITOR_TIMELINE_ROW_GAP_PT;
  }
  return height;
}

float32_t vkr_editor_recording_timeline_height(VkrEditorUi *editor) {
  const VkrEditorRecordingTimeline *timeline = &editor->recording.timeline;
  return timeline->open
             ? editor_timeline_panel_height(timeline) + EDITOR_TIMELINE_GAP_PT
             : 0.0f;
}

/* ------------------------------------------------------------------------ */
/* View and columns                                                          */

/* A new recording shows all of itself, unselected. */
vkr_internal void editor_timeline_reset(VkrEditorRecordingTimeline *timeline,
                                        const VkrRecordingPlayback *playback) {
  if (string_equals(timeline->bundle, playback->bundle_path)) {
    return;
  }
  string_format(timeline->bundle, sizeof(timeline->bundle), "%s",
                playback->bundle_path);
  timeline->view_first = 0u;
  timeline->view_last = playback->frame_count - 1u;
  timeline->selected = false_v;
  timeline->column_count = 0u;
  timeline->hovered_column = VKR_UI_PLOT_NONE;
  timeline->memory_hovered = VKR_UI_PLOT_NONE;
  timeline->system_hovered = VKR_UI_PLOT_NONE;
}

/* Sets the view to `first` + `span` frames, kept inside the recording. */
vkr_internal void editor_timeline_view(VkrEditorRecordingTimeline *timeline,
                                       const VkrRecordingPlayback *playback,
                                       int64_t first, uint64_t span) {
  const uint64_t count = playback->frame_count;
  span = Min(Max(span, (uint64_t)EDITOR_TIMELINE_MIN_FRAMES), count);
  first = Max(first, (int64_t)0);
  first = Min(first, (int64_t)(count - span));
  timeline->view_first = (uint64_t)first;
  timeline->view_last = (uint64_t)first + span - 1u;
}

void vkr_editor_recording_timeline_show(VkrEditorUi *editor,
                                        const VkrRecordingPlayback *playback,
                                        uint64_t first, uint64_t last) {
  VkrEditorRecordingTimeline *timeline = &editor->recording.timeline;
  if (playback->frame_count == 0u) {
    return;
  }
  editor_timeline_reset(timeline, playback);
  timeline->open = true_v;
  if (first == UINT64_MAX) {
    editor_timeline_view(timeline, playback, 0, playback->frame_count);
    return;
  }
  const uint64_t span = last - first + 1u;
  const uint64_t margin = Max(span / 10u, (uint64_t)1u);
  editor_timeline_view(timeline, playback, (int64_t)first - (int64_t)margin,
                       span + 2u * margin);
}

vkr_internal uint64_t
editor_timeline_span(const VkrEditorRecordingTimeline *timeline) {
  return timeline->view_last - timeline->view_first + 1u;
}

/* The first frame column `column` covers. */
vkr_internal uint64_t editor_timeline_column_start(
    const VkrEditorRecordingTimeline *timeline, uint32_t column) {
  return timeline->view_first + (uint64_t)column *
                                    editor_timeline_span(timeline) /
                                    timeline->column_count;
}

/* The column holding `frame`, or VKR_UI_PLOT_NONE outside the view. */
vkr_internal uint32_t editor_timeline_column_of(
    const VkrEditorRecordingTimeline *timeline, uint64_t frame) {
  if (frame < timeline->view_first || frame > timeline->view_last ||
      timeline->column_count == 0u) {
    return VKR_UI_PLOT_NONE;
  }
  return (uint32_t)((frame - timeline->view_first) * timeline->column_count /
                    editor_timeline_span(timeline));
}

/* Each column's highest resident and GPU driver memory, in GiB. A column
   without a sample carries the one before it, so the lines stay level
   over lost or unsampled frames. */
vkr_internal void
editor_timeline_memory_columns(VkrEditorRecordingTimeline *timeline,
                               const VkrRecordingPlayback *playback) {
  if (!playback->has_memory) {
    return;
  }
  float32_t *columns[2] = {timeline->column_ram, timeline->column_vram};
  for (uint32_t m = 0u; m < 2u; ++m) {
    float32_t carried = -1.0f;
    timeline->memory_min_gb[m] = timeline->memory_max_gb[m] = -1.0f;
    for (uint32_t c = 0u; c < timeline->column_count; ++c) {
      const uint64_t first = editor_timeline_column_start(timeline, c);
      const uint64_t end = c + 1u < timeline->column_count
                               ? editor_timeline_column_start(timeline, c + 1u)
                               : timeline->view_last + 1u;
      float32_t highest = -1.0f;
      for (uint64_t f = first; f < end; ++f) {
        if (playback->memory_valid[f] & (1u << m)) {
          highest =
              Max(highest, (float32_t)(playback->memory_bytes[f * 2u + m] /
                                       (1024.0 * 1024.0 * 1024.0)));
        }
      }
      carried = highest >= 0.0f ? highest : carried;
      columns[m][c] = carried;
      if (carried >= 0.0f) {
        timeline->memory_min_gb[m] =
            timeline->memory_min_gb[m] < 0.0f
                ? carried
                : Min(timeline->memory_min_gb[m], carried);
        timeline->memory_max_gb[m] = Max(timeline->memory_max_gb[m], carried);
      }
    }
    /* Columns before the first sample take it. */
    for (uint32_t c = 0u; c < timeline->column_count && columns[m][c] < 0.0f;
         ++c) {
      columns[m][c] = Max(timeline->memory_min_gb[m], 0.0f);
    }
  }
}

/* Each column's worst thermal level, memory pressure and low-power state,
   the errors any of its frames counted, and its highest footprint, from the
   playback's platform fields (ADR-125). A recording without them, as from
   before them, has no System lane. */
vkr_internal void
editor_timeline_system_columns(VkrEditorRecordingTimeline *timeline,
                               const VkrRecordingPlayback *playback) {
  timeline->system_source = playback->system_values;
  timeline->system_lane = playback->has_system;
  timeline->system_errors = false_v;
  if (!playback->has_system) {
    return;
  }
  static const VkrRecordingSystemField level_fields[] = {
      VKR_RECORDING_SYSTEM_THERMAL, VKR_RECORDING_SYSTEM_MEMORY_PRESSURE,
      VKR_RECORDING_SYSTEM_LOW_POWER};
  static const uint8_t level_max[] = {3u, 2u, 1u};
  /* Bit 0 GPU errors, bit 1 log errors: both count since the frame before. */
  static const VkrRecordingSystemField error_fields[] = {
      VKR_RECORDING_SYSTEM_GPU_ERRORS, VKR_RECORDING_SYSTEM_LOG_ERRORS};
  const uint32_t stride = VKR_RECORDING_SYSTEM_FIELDS;
  const uint64_t frame_count = playback->frame_count;

  const uint32_t error_row = VKR_EDITOR_TIMELINE_SYSTEM_ROWS - 1u;
  MemZero(timeline->system_worst, sizeof(timeline->system_worst));
  for (uint32_t c = 0u; c < timeline->column_count; ++c) {
    const uint64_t first = editor_timeline_column_start(timeline, c);
    const uint64_t end =
        Min(c + 1u < timeline->column_count
                ? editor_timeline_column_start(timeline, c + 1u)
                : timeline->view_last + 1u,
            frame_count);
    uint8_t worst[VKR_EDITOR_TIMELINE_SYSTEM_ROWS];
    MemSet(worst, VKR_EDITOR_TIMELINE_SYSTEM_NONE, sizeof(worst));
    float64_t highest = -1.0;
    for (uint64_t f = first; f < end; ++f) {
      const uint8_t valid = playback->system_valid[f];
      const float64_t *values = &playback->system_values[f * stride];
      for (uint32_t r = 0u; r < ArrayCount(level_fields); ++r) {
        const uint32_t k = (uint32_t)level_fields[r];
        if (!(valid & (1u << k)) || !vkr_is_finite_f64(values[k])) {
          continue;
        }
        /* Levels are small whole numbers; a value past the top is the
           top. */
        const float64_t value =
            vkr_clamp_f64(values[k], 0.0, (float64_t)level_max[r]);
        const uint8_t level = (uint8_t)(value + 0.5);
        worst[r] = worst[r] == VKR_EDITOR_TIMELINE_SYSTEM_NONE
                       ? level
                       : Max(worst[r], level);
      }
      for (uint32_t e = 0u; e < ArrayCount(error_fields); ++e) {
        const uint32_t k = (uint32_t)error_fields[e];
        if (!(valid & (1u << k))) {
          continue;
        }
        timeline->system_errors = true_v;
        if (worst[error_row] == VKR_EDITOR_TIMELINE_SYSTEM_NONE) {
          worst[error_row] = 0u;
        }
        if (values[k] > 0.0) {
          worst[error_row] |= (uint8_t)(1u << e);
        }
      }
      const uint32_t footprint = VKR_RECORDING_SYSTEM_FOOTPRINT;
      if (valid & (1u << footprint)) {
        highest = Max(highest, values[footprint]);
      }
    }
    for (uint32_t r = 0u; r < VKR_EDITOR_TIMELINE_SYSTEM_ROWS; ++r) {
      const bool8_t drawn =
          worst[r] != VKR_EDITOR_TIMELINE_SYSTEM_NONE && worst[r] > 0u;
      timeline->system_level[r][c] = worst[r];
      timeline->system_band[r][c] = drawn ? 1.0f : 0.0f;
      if (drawn) {
        timeline->system_worst[r] =
            r == error_row ? timeline->system_worst[r] | worst[r]
                           : Max(timeline->system_worst[r], worst[r]);
      }
    }
    timeline->system_footprint[c] = (float32_t)highest;
  }
}

/* Each column's slowest frame over the view, built when the view or the
   System lane's data changes. */
vkr_internal void
editor_timeline_columns(VkrEditorRecordingTimeline *timeline,
                        const VkrRecordingPlayback *playback) {
  if (timeline->column_count > 0u &&
      timeline->columns_first == timeline->view_first &&
      timeline->columns_last == timeline->view_last &&
      timeline->system_source == playback->system_values) {
    return;
  }
  const float64_t budget = VKR_RECORDING_ANALYSIS_BUDGET_MS;
  const uint64_t span = editor_timeline_span(timeline);
  timeline->column_count =
      (uint32_t)Min(span, (uint64_t)VKR_EDITOR_TIMELINE_COLUMNS);
  timeline->columns_first = timeline->view_first;
  timeline->columns_last = timeline->view_last;
  float32_t slowest = 0.0f;
  for (uint32_t c = 0u; c < timeline->column_count; ++c) {
    const uint64_t first = editor_timeline_column_start(timeline, c);
    const uint64_t end = c + 1u < timeline->column_count
                             ? editor_timeline_column_start(timeline, c + 1u)
                             : timeline->view_last + 1u;
    uint64_t worst = first;
    for (uint64_t f = first; f < end; ++f) {
      if (playback->frames[f].display_dt > playback->frames[worst].display_dt) {
        worst = f;
      }
    }
    const float64_t ms = playback->frames[worst].display_dt * 1000.0;
    timeline->column_frame[c] = worst;
    timeline->column_ms[c] = (float32_t)ms;
    timeline->column_class[c] = ms > 2.0 * budget ? 2u : ms > budget ? 1u : 0u;
    slowest = Max(slowest, (float32_t)ms);
  }
  editor_timeline_memory_columns(timeline, playback);
  editor_timeline_system_columns(timeline, playback);
  /* Room above the slowest bar, and the budget line always in view. */
  timeline->top_ms = Max(slowest * 1.08f, (float32_t)budget * 1.25f);
}

/* ------------------------------------------------------------------------ */
/* Header                                                                    */

vkr_internal VkrUiPlacement editor_timeline_cell(uint32_t column,
                                                 uint32_t row) {
  return (VkrUiPlacement){.column = column,
                          .row = row,
                          .column_span = 1u,
                          .row_span = 1u,
                          .justify = VKR_UI_ALIGN_STRETCH,
                          .align = VKR_UI_ALIGN_CENTER};
}

/* A compact text button for the header's actions. */
vkr_internal VkrUiWidgetConfig editor_timeline_button(VkrEditorUi *editor,
                                                      uint32_t column,
                                                      VkrUiIcon icon,
                                                      String8 tooltip) {
  VkrUiWidgetConfig button = vkr_ui_widget_config_default();
  button.placement = editor_timeline_cell(column, 0u);
  vkr_editor_action_style(&button, editor->heading_font);
  button.icon = icon;
  vkr_editor_button_size(&button);
  button.style.min_size_pt.y = button.style.max_size_pt.y = 22.0f;
  button.style.padding_pt.left = button.style.padding_pt.right = 8.0f;
  button.style.font_size_pt = vkr_ui_theme()->font_caption;
  button.icon_size_pt = 11.0f;
  button.tooltip = tooltip;
  return button;
}

/* Loops the playback over the selection, or stops looping it. */
vkr_internal void editor_timeline_loop(VkrEditorUi *editor,
                                       const VkrRecordingPlayback *playback,
                                       bool8_t looping) {
  const VkrEditorRecordingTimeline *timeline = &editor->recording.timeline;
  if (looping) {
    vkr_editor_recording_settings(editor, playback->speed, false_v, 0.0, 0.0);
    return;
  }
  vkr_editor_recording_settings(editor, playback->speed, true_v,
                                playback->times[timeline->select_first],
                                playback->times[timeline->select_last]);
}

/* Writes a perf re-run of the selection and copies its command. */
vkr_internal void editor_timeline_rerun(VkrEditorUi *editor,
                                        const VkrRecordingPlayback *playback) {
  const VkrEditorRecordingTimeline *timeline = &editor->recording.timeline;
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrEditorRecordingRerun written;
  char error[192];
  if (!vkr_editor_recording_rerun(editor, playback->bundle_path,
                                  timeline->select_first, timeline->select_last,
                                  &written, error, sizeof(error))) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, theme->warning, error);
    return;
  }
  (void)vkr_platform_clipboard_write_text(
      (const uint8_t *)written.command,
      (uint32_t)string_length(written.command));
  char text[VKR_RECORDING_PATH_MAX + 64u];
  string_format(text, sizeof(text),
                "Wrote %s; its harness command is on the clipboard.",
                written.case_path);
  vkr_editor_toast(editor, VKR_UI_ICON_SPEED, theme->text, text);
}

/* The selection and its actions: loop, mark as a segment, re-run, zoom to,
   clear. */
vkr_internal void
editor_timeline_selection(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                          const VkrRecordingPlayback *playback) {
  VkrEditorRecordingTimeline *timeline = &editor->recording.timeline;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack columns[] = {auto_track, auto_track, auto_track,
                                auto_track, auto_track, auto_track};
  const VkrUiTrack row = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = editor_timeline_cell(1u, 0u);
  panel.columns = columns;
  panel.column_count = ArrayCount(columns);
  panel.rows = &row;
  panel.row_count = 1u;
  panel.style.gap_pt = theme->space_sm;
  if (!vkr_ui_panel_begin(ui, string8_lit("selection"), &panel)) {
    return;
  }
  const float64_t seconds = playback->times[timeline->select_last] -
                            playback->times[timeline->select_first];
  VkrUiWidgetConfig chip =
      vkr_editor_text_config(theme->font_caption, theme->text);
  chip.placement = editor_timeline_cell(0u, 0u);
  chip.style.padding_pt.right = theme->space_sm;
  vkr_ui_label(ui, string8_lit("chip"),
               string8_create_formatted(
                   ui->frame_allocator,
                   "Frames %llu\xe2\x80\x93%llu  \xc2\xb7  %.2f s",
                   (unsigned long long)timeline->select_first,
                   (unsigned long long)timeline->select_last, seconds),
               &chip);
  const bool8_t looping =
      playback->loop && playback->loop_end > playback->loop_start &&
      playback->loop_start == playback->times[timeline->select_first] &&
      playback->loop_end == playback->times[timeline->select_last];
  VkrUiWidgetConfig loop = editor_timeline_button(
      editor, 1u, VKR_UI_ICON_REFRESH,
      looping ? string8_lit("Stop looping the selection")
              : string8_lit("Loop playback over the selection"));
  vkr_editor_toggle_style(&loop, looping);
  if (vkr_ui_button(ui, string8_lit("loop"), string8_lit("Loop"), &loop)) {
    editor_timeline_loop(editor, playback, looping);
  }
  VkrUiWidgetConfig segment = editor_timeline_button(
      editor, 2u, VKR_UI_ICON_RULER,
      string8_lit("Mark the selection as a segment and say why it matters"));
  if (vkr_ui_button(ui, string8_lit("segment"), string8_lit("Segment"),
                    &segment) &&
      vkr_editor_recording_segment_add(editor, playback, timeline->select_first,
                                       timeline->select_last)) {
    timeline->selected = false_v;
  }
  VkrUiWidgetConfig rerun = editor_timeline_button(
      editor, 3u, VKR_UI_ICON_SPEED,
      string8_lit("Benchmark the selection: write a harness case that "
                  "replays it and copy its command"));
  if (vkr_ui_button(ui, string8_lit("rerun"), string8_lit("Re-run"), &rerun)) {
    editor_timeline_rerun(editor, playback);
  }
  VkrUiWidgetConfig zoom = editor_timeline_button(
      editor, 4u, VKR_UI_ICON_ZOOM_IN, string8_lit("Show only the selection"));
  if (vkr_ui_button(ui, string8_lit("zoom"), string8_lit("Zoom"), &zoom)) {
    editor_timeline_view(timeline, playback, (int64_t)timeline->select_first,
                         timeline->select_last - timeline->select_first + 1u);
  }
  VkrUiWidgetConfig clear = vkr_editor_icon_button_config(
      5u, 0u, VKR_UI_ICON_CLOSE, string8_lit("Clear the selection"));
  if (vkr_ui_button(ui, string8_lit("clear"), (String8){0}, &clear)) {
    timeline->selected = false_v;
  }
  (void)vkr_ui_panel_end(ui);
}

/* The view's caption, the selection's actions and the zoom buttons. */
vkr_internal void editor_timeline_header(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const VkrRecordingPlayback *playback) {
  VkrEditorRecordingTimeline *timeline = &editor->recording.timeline;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack columns[] = {{.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                                auto_track,
                                {.value = 24.0f, .unit = VKR_UI_TRACK_PX},
                                {.value = 24.0f, .unit = VKR_UI_TRACK_PX},
                                {.value = 24.0f, .unit = VKR_UI_TRACK_PX}};
  const VkrUiTrack row = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig header = vkr_ui_panel_config_default();
  header.placement = editor_timeline_cell(0u, 0u);
  header.columns = columns;
  header.column_count = ArrayCount(columns);
  header.rows = &row;
  header.row_count = 1u;
  header.style.gap_pt = theme->space_xs;
  if (!vkr_ui_panel_begin(ui, string8_lit("header"), &header)) {
    return;
  }
  VkrUiWidgetConfig caption =
      vkr_editor_section_label_config(editor->heading_font);
  caption.placement = editor_timeline_cell(0u, 0u);
  caption.placement.justify = VKR_UI_ALIGN_START;
  caption.tooltip = string8_lit("Scroll over the frame times to zoom, with "
                                "Shift to pan; drag across them to select");
  vkr_ui_label(ui, string8_lit("caption"),
               string8_create_formatted(
                   ui->frame_allocator,
                   "Timeline  \xc2\xb7  frames %llu\xe2\x80\x93%llu of %llu",
                   (unsigned long long)timeline->view_first,
                   (unsigned long long)timeline->view_last,
                   (unsigned long long)playback->frame_count),
               &caption);
  if (timeline->selected) {
    editor_timeline_selection(editor, frame, playback);
  }
  const uint64_t span = editor_timeline_span(timeline);
  const int64_t middle = (int64_t)timeline->view_first + (int64_t)(span / 2u);
  VkrUiWidgetConfig out = vkr_editor_icon_button_config(
      2u, 0u, VKR_UI_ICON_ZOOM_OUT, string8_lit("Zoom out"));
  if (vkr_ui_button(ui, string8_lit("out"), (String8){0}, &out)) {
    const uint64_t wider = span * 2u;
    editor_timeline_view(timeline, playback, middle - (int64_t)(wider / 2u),
                         wider);
  }
  VkrUiWidgetConfig fit = vkr_editor_icon_button_config(
      3u, 0u, VKR_UI_ICON_MAXIMIZE, string8_lit("Show the whole recording"));
  if (vkr_ui_button(ui, string8_lit("fit"), (String8){0}, &fit)) {
    editor_timeline_view(timeline, playback, 0, playback->frame_count);
  }
  VkrUiWidgetConfig in = vkr_editor_icon_button_config(
      4u, 0u, VKR_UI_ICON_ZOOM_IN, string8_lit("Zoom in"));
  if (vkr_ui_button(ui, string8_lit("in"), (String8){0}, &in)) {
    const uint64_t narrower = span / 2u;
    editor_timeline_view(timeline, playback, middle - (int64_t)(narrower / 2u),
                         narrower);
  }
  (void)vkr_ui_panel_end(ui);
}

/* ------------------------------------------------------------------------ */
/* Lanes                                                                     */

/* Zooms around the hovered column with the wheel, or pans with Shift. */
vkr_internal void editor_timeline_wheel(VkrEditorRecordingTimeline *timeline,
                                        const VkrRecordingPlayback *playback,
                                        VkrUiSystem *ui, uint32_t hovered) {
  if (hovered == VKR_UI_PLOT_NONE || ui->mouse_wheel == 0) {
    return;
  }
  const uint64_t span = editor_timeline_span(timeline);
  const bool8_t shift = input_is_key_down(ui->input, KEY_LSHIFT) ||
                        input_is_key_down(ui->input, KEY_RSHIFT);
  if (shift) {
    const int64_t step = Max((int64_t)(span / 8u), (int64_t)1);
    editor_timeline_view(
        timeline, playback,
        (int64_t)timeline->view_first - (int64_t)ui->mouse_wheel * step, span);
    return;
  }
  /* The frame under the pointer stays under it. */
  const uint64_t anchor = editor_timeline_column_start(timeline, hovered);
  const float64_t factor = ui->mouse_wheel > 0
                               ? (float64_t)EDITOR_TIMELINE_ZOOM
                               : 1.0 / (float64_t)EDITOR_TIMELINE_ZOOM;
  const uint64_t next = (uint64_t)vkr_ceil_f64((float64_t)span * factor);
  const float64_t share =
      (float64_t)(anchor - timeline->view_first) / (float64_t)span;
  editor_timeline_view(timeline, playback,
                       (int64_t)anchor - (int64_t)(share * (float64_t)next),
                       next);
}

/* The frame-time lane: bars against the budget, the playhead, the selection
   or loop span; hover reads a frame, a click seeks, a drag selects. */
vkr_internal void editor_timeline_plot(VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       const VkrRecordingPlayback *playback) {
  VkrEditorRecordingTimeline *timeline = &editor->recording.timeline;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float64_t budget = VKR_RECORDING_ANALYSIS_BUDGET_MS;
  const Vec4 palette[] = {theme->success, theme->warning, theme->error};
  const VkrUiPlotSeries bars = {.values = timeline->column_ms,
                                .color = theme->success};
  const VkrUiPlotReference references[] = {
      {.value = (float32_t)budget,
       .color = vkr_ui_color_alpha(theme->text_secondary, 0.7f),
       .dashed = true_v},
  };
  const uint64_t current =
      vkr_recording_playback_frame_at(playback, playback->time);
  /* The selection, else the loop span, shows behind the bars. */
  uint32_t range_first = VKR_UI_PLOT_NONE;
  uint32_t range_last = VKR_UI_PLOT_NONE;
  Vec4 range_color = vkr_ui_color_alpha(theme->accent_hover, 0.22f);
  if (timeline->selected) {
    range_first = editor_timeline_column_of(timeline, timeline->select_first);
    range_last = editor_timeline_column_of(timeline, timeline->select_last);
  } else if (playback->loop && playback->loop_end > playback->loop_start) {
    range_first = editor_timeline_column_of(
        timeline,
        vkr_recording_playback_frame_at(playback, playback->loop_start));
    range_last = editor_timeline_column_of(
        timeline,
        vkr_recording_playback_frame_at(playback, playback->loop_end));
    range_color = vkr_ui_color_alpha(theme->warning, 0.18f);
  }
  VkrUiPlotConfig plot = {
      .sample_count = timeline->column_count,
      .y_min = 0.0f,
      .y_max = timeline->top_ms,
      .bars = &bars,
      .bar_count = 1u,
      .bar_colors = timeline->column_class,
      .palette = palette,
      .palette_count = ArrayCount(palette),
      .references = references,
      .reference_count = ArrayCount(references),
      .cursor = editor_timeline_column_of(timeline, current),
      .cursor_color = theme->text,
      .range_first = range_first,
      .range_last = range_last,
      .range_color = range_color,
      .hover_color = vkr_ui_color_alpha(theme->text, 0.08f),
  };
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement = editor_timeline_cell(0u, 1u);
  config.placement.align = VKR_UI_ALIGN_STRETCH;
  config.style.background_color = vkr_ui_color_alpha(theme->window, 0.6f);
  config.style.corner_radius_pt = (Vec4){4.0f, 4.0f, 4.0f, 4.0f};
  /* A recording with thumbnails shows the frame in the hover preview. */
  const uint32_t hovered = timeline->hovered_column;
  if (hovered != VKR_UI_PLOT_NONE && hovered < timeline->column_count &&
      !timeline->previewed) {
    const uint64_t at = timeline->column_frame[hovered];
    config.tooltip = string8_create_formatted(
        ui->frame_allocator, "Frame %llu  \xc2\xb7  %.2f ms  \xc2\xb7  %.2f s",
        (unsigned long long)at, timeline->column_ms[hovered],
        playback->times[at]);
  }
  const VkrUiPlotResult result = vkr_ui_plot(
      ui, string8_lit("editor.recording.timeline.plot"), &plot, &config);
  timeline->plot_id = result.id;
  timeline->hovered_column = result.hovered;
  if (result.clicked != VKR_UI_PLOT_NONE) {
    VkrSampleRecordingRequest seek = {
        .command = VKR_SAMPLE_RECORDING_SEEK_FRAME,
        .frame = editor_timeline_column_start(timeline, result.clicked)};
    (void)vkr_editor_recording_send(editor, seek, false_v);
  }
  if (result.brushing || result.brushed) {
    const uint32_t last_column = result.brush_last;
    timeline->selected = true_v;
    timeline->select_first =
        editor_timeline_column_start(timeline, result.brush_first);
    timeline->select_last =
        last_column + 1u < timeline->column_count
            ? editor_timeline_column_start(timeline, last_column + 1u) - 1u
            : timeline->view_last;
  }
  editor_timeline_wheel(timeline, playback, ui, result.hovered);
}

/* Resident (RAM) and GPU driver memory per column as two lines on their
   own axis, labelled in the lane's corner; hover reads a column, a click
   seeks. On Apple silicon GPU memory is part of the resident footprint, so
   the lines are never added. */
vkr_internal void editor_timeline_memory(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const VkrRecordingPlayback *playback,
                                         uint32_t row) {
  VkrEditorRecordingTimeline *timeline = &editor->recording.timeline;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const Vec4 colors[2] = {theme->accent, {0.66f, 0.55f, 0.98f, 1.0f}};
  /* Each line on its own axis, so a change of a few MB shows; the lines
     keep the lower part of the lane and the legend the top. */
  VkrUiPlotSeries lines[2];
  uint32_t line_count = 0u;
  for (uint32_t m = 0u; m < 2u; ++m) {
    if (timeline->memory_max_gb[m] < 0.0f) {
      continue;
    }
    const float32_t low = timeline->memory_min_gb[m];
    const float32_t span = Max(timeline->memory_max_gb[m] - low, 0.02f);
    lines[line_count++] = (VkrUiPlotSeries){
        .values = m == 0u ? timeline->column_ram : timeline->column_vram,
        .color = colors[m],
        .y_min = low - span * 0.15f,
        .y_max = low + span * 1.9f,
    };
  }
  const uint64_t current =
      vkr_recording_playback_frame_at(playback, playback->time);
  const VkrUiPlotConfig plot = {
      .sample_count = timeline->column_count,
      .y_min = 0.0f,
      .y_max = 1.0f,
      .lines = lines,
      .line_count = line_count,
      .cursor = editor_timeline_column_of(timeline, current),
      .cursor_color = theme->text,
      .range_first = VKR_UI_PLOT_NONE,
      .range_last = VKR_UI_PLOT_NONE,
      .hover_color = vkr_ui_color_alpha(theme->text, 0.08f),
  };
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement = editor_timeline_cell(0u, row);
  config.placement.align = VKR_UI_ALIGN_STRETCH;
  config.style.background_color = vkr_ui_color_alpha(theme->window, 0.6f);
  config.style.corner_radius_pt = (Vec4){4.0f, 4.0f, 4.0f, 4.0f};
  const uint32_t hovered = timeline->memory_hovered;
  if (hovered != VKR_UI_PLOT_NONE && hovered < timeline->column_count) {
    config.tooltip = string8_create_formatted(
        ui->frame_allocator,
        "Frame %llu  \xc2\xb7  RAM %.2f GiB  \xc2\xb7  GPU %.2f GiB",
        (unsigned long long)editor_timeline_column_start(timeline, hovered),
        timeline->column_ram[hovered], timeline->column_vram[hovered]);
  }
  const VkrUiPlotResult result = vkr_ui_plot(
      ui, string8_lit("editor.recording.timeline.memory"), &plot, &config);
  timeline->memory_id = result.id;
  timeline->memory_hovered = result.hovered;
  if (result.clicked != VKR_UI_PLOT_NONE) {
    VkrSampleRecordingRequest seek = {
        .command = VKR_SAMPLE_RECORDING_SEEK_FRAME,
        .frame = editor_timeline_column_start(timeline, result.clicked)};
    (void)vkr_editor_recording_send(editor, seek, false_v);
  }
  /* The legend, over the lane's top-left corner: each line's name and the
     range it spans in the view. */
  const char *names[2] = {"RAM", "GPU"};
  float32_t left = 6.0f;
  for (uint32_t m = 0u; m < 2u; ++m) {
    if (timeline->memory_max_gb[m] < 0.0f) {
      continue;
    }
    VkrUiWidgetConfig label =
        vkr_editor_text_config(theme->font_caption, theme->text_secondary);
    label.placement = editor_timeline_cell(0u, row);
    label.placement.justify = VKR_UI_ALIGN_START;
    label.placement.align = VKR_UI_ALIGN_START;
    label.placement.margin_pt = (VkrUiEdges){2.0f, 0.0f, 0.0f, left};
    label.icon = VKR_UI_ICON_DOT;
    label.icon_size_pt = 7.0f;
    label.icon_color = colors[m];
    (void)vkr_ui_push_id_u64(ui, m);
    /* Under 100 MiB of change reads in MiB, so it is not lost to rounding;
       a level line reads one value. */
    const float32_t low = timeline->memory_min_gb[m];
    const float32_t high = timeline->memory_max_gb[m];
    const bool8_t megabytes = (high - low) * 1024.0f < 100.0f;
    const float32_t scale = megabytes ? 1024.0f : 1.0f;
    const char *unit = megabytes ? "MiB" : "GiB";
    char low_text[32];
    char high_text[32];
    string_format(low_text, sizeof(low_text), megabytes ? "%.0f" : "%.2f",
                  low * scale);
    string_format(high_text, sizeof(high_text), megabytes ? "%.0f" : "%.2f",
                  high * scale);
    vkr_ui_label(ui, string8_lit("memory.legend"),
                 string_equals(low_text, high_text)
                     ? string8_create_formatted(ui->frame_allocator, "%s %s %s",
                                                names[m], low_text, unit)
                     : string8_create_formatted(
                           ui->frame_allocator, "%s %s\xe2\x80\x93%s %s",
                           names[m], low_text, high_text, unit),
                 &label);
    (void)vkr_ui_pop_id(ui);
    left += 150.0f;
  }
}

/* Appends `word` to `text` after a separator, keeping it terminated when
   it runs out of room. */
vkr_internal void editor_timeline_append(char *text, uint64_t capacity,
                                         int32_t *length, const char *word) {
  if (*length < 0 || (uint64_t)*length >= capacity) {
    return;
  }
  *length += string_format(text + *length, capacity - (uint64_t)*length,
                           *length > 0 ? "  \xc2\xb7  %s" : "%s", word);
}

/* The words of a System row's `level`: the thermal level, the memory
   pressure, low-power mode, or which errors a frame counted. */
vkr_internal const char *editor_timeline_system_word(uint32_t row,
                                                     uint8_t level) {
  static const char *const thermal[] = {"Thermal nominal", "Thermal fair",
                                        "Thermal serious", "Thermal critical"};
  static const char *const pressure[] = {"Memory normal", "Memory warn",
                                         "Memory critical"};
  static const char *const power[] = {"Low power off", "Low power on"};
  static const char *const errors[] = {"No errors", "GPU errors", "Log errors",
                                       "GPU and log errors"};
  switch (row) {
  case 0u:
    return thermal[Min(level, 3u)];
  case 1u:
    return pressure[Min(level, 2u)];
  case 2u:
    return power[Min(level, 1u)];
  default:
    return errors[level & 3u];
  }
}

/* Platform signals per column as thin bands under a legend of the view's
   worst: thermal level, memory pressure and low-power mode, coloured by
   severity and left empty when nominal, then ticks where frames counted
   GPU (critical colour) or log (warning colour) errors. Hover reads a
   column, a click seeks. */
vkr_internal void editor_timeline_system(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const VkrRecordingPlayback *playback,
                                         uint32_t row) {
  VkrEditorRecordingTimeline *timeline = &editor->recording.timeline;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const uint64_t current =
      vkr_recording_playback_frame_at(playback, playback->time);
  const uint32_t cursor = editor_timeline_column_of(timeline, current);
  const uint32_t rows = editor_timeline_system_rows(timeline);

  /* The hovered column: its frames, then each signal it sampled. */
  String8 tooltip = {0};
  const uint32_t hovered = timeline->system_hovered;
  if (hovered != VKR_UI_PLOT_NONE && hovered < timeline->column_count) {
    const uint64_t first = editor_timeline_column_start(timeline, hovered);
    const uint64_t last =
        hovered + 1u < timeline->column_count
            ? editor_timeline_column_start(timeline, hovered + 1u) - 1u
            : timeline->view_last;
    char words[192];
    int32_t length = 0;
    words[0] = '\0';
    for (uint32_t r = 0u; r < rows; ++r) {
      const uint8_t level = timeline->system_level[r][hovered];
      if (level != VKR_EDITOR_TIMELINE_SYSTEM_NONE) {
        editor_timeline_append(words, sizeof(words), &length,
                               editor_timeline_system_word(r, level));
      }
    }
    /* The footprint takes its own line, in the memory lane's binary
       units. */
    const float32_t bytes = timeline->system_footprint[hovered];
    char footprint[48] = {0};
    if (bytes >= 0.0f) {
      const float32_t gib = bytes / (1024.0f * 1024.0f * 1024.0f);
      string_format(footprint, sizeof(footprint),
                    gib >= 1.0f ? "\nFootprint %.1f GiB"
                                : "\nFootprint %.0f MiB",
                    gib >= 1.0f ? gib : bytes / (1024.0f * 1024.0f));
    }
    tooltip =
        first == last
            ? string8_create_formatted(ui->frame_allocator, "Frame %llu\n%s%s",
                                       (unsigned long long)first, words,
                                       footprint)
            : string8_create_formatted(
                  ui->frame_allocator, "Frames %llu\xe2\x80\x93%llu\n%s%s",
                  (unsigned long long)first, (unsigned long long)last, words,
                  footprint);
  }

  /* The lane: its background, the playhead, hover and a click to seek. */
  const VkrUiPlotConfig lane_plot = {
      .sample_count = timeline->column_count,
      .y_min = 0.0f,
      .y_max = 1.0f,
      .cursor = cursor,
      .cursor_color = theme->text,
      .range_first = VKR_UI_PLOT_NONE,
      .range_last = VKR_UI_PLOT_NONE,
      .hover_color = vkr_ui_color_alpha(theme->text, 0.08f),
  };
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement = editor_timeline_cell(0u, row);
  config.placement.align = VKR_UI_ALIGN_STRETCH;
  config.style.background_color = vkr_ui_color_alpha(theme->window, 0.6f);
  config.style.corner_radius_pt = (Vec4){4.0f, 4.0f, 4.0f, 4.0f};
  config.tooltip = tooltip;
  const VkrUiPlotResult result = vkr_ui_plot(
      ui, string8_lit("editor.recording.timeline.system"), &lane_plot, &config);
  timeline->system_hovered = result.hovered;
  uint32_t clicked = result.clicked;

  /* One band per row, over the lane, with its columns' severity colours.
     Each shares the lane's columns, tooltip and playhead, and a press on
     it lands on it, so it seeks too. */
  const Vec4 faint = vkr_ui_color_alpha(theme->warning, 0.45f);
  const Vec4 none = {0};
  const Vec4 palettes[VKR_EDITOR_TIMELINE_SYSTEM_ROWS][4] = {
      {none, faint, theme->warning, theme->error},
      {none, faint, theme->error, theme->error},
      {none, theme->warning, theme->warning, theme->warning},
      {none, theme->error, theme->warning, theme->error},
  };
  for (uint32_t r = 0u; r < rows; ++r) {
    const VkrUiPlotSeries band = {.values = timeline->system_band[r],
                                  .color = theme->warning};
    const VkrUiPlotConfig band_plot = {
        .sample_count = timeline->column_count,
        .y_min = 0.0f,
        .y_max = 1.0f,
        .bars = &band,
        .bar_count = 1u,
        .bar_colors = timeline->system_level[r],
        .palette = palettes[r],
        .palette_count = ArrayCount(palettes[r]),
        .cursor = cursor,
        .cursor_color = theme->text,
        .range_first = VKR_UI_PLOT_NONE,
        .range_last = VKR_UI_PLOT_NONE,
    };
    VkrUiWidgetConfig track = vkr_ui_widget_config_default();
    track.placement = editor_timeline_cell(0u, row);
    track.placement.align = VKR_UI_ALIGN_START;
    track.placement.margin_pt.top =
        EDITOR_TIMELINE_SYSTEM_TOP_PT +
        (float32_t)r * (EDITOR_TIMELINE_SYSTEM_BAND_PT +
                        EDITOR_TIMELINE_SYSTEM_BAND_GAP_PT);
    track.style.min_size_pt.y = track.style.max_size_pt.y =
        EDITOR_TIMELINE_SYSTEM_BAND_PT;
    track.style.background_color = vkr_ui_color_alpha(theme->text, 0.06f);
    track.style.corner_radius_pt = (Vec4){1.5f, 1.5f, 1.5f, 1.5f};
    track.tooltip = tooltip;
    (void)vkr_ui_push_id_u64(ui, r);
    const VkrUiPlotResult band_result =
        vkr_ui_plot(ui, string8_lit("editor.recording.timeline.system.band"),
                    &band_plot, &track);
    (void)vkr_ui_pop_id(ui);
    if (band_result.clicked != VKR_UI_PLOT_NONE) {
      clicked = band_result.clicked;
    }
  }
  if (clicked != VKR_UI_PLOT_NONE) {
    VkrSampleRecordingRequest seek = {
        .command = VKR_SAMPLE_RECORDING_SEEK_FRAME,
        .frame = editor_timeline_column_start(timeline, clicked)};
    (void)vkr_editor_recording_send(editor, seek, false_v);
  }

  /* The legend, over the lane's top-left corner like the memory lane's:
     its name, then the view's worst of each signal that is not nominal. */
  char legend[192];
  int32_t length = 0;
  legend[0] = '\0';
  editor_timeline_append(legend, sizeof(legend), &length, "System");
  bool8_t flagged = false_v;
  for (uint32_t r = 0u; r < rows; ++r) {
    if (timeline->system_worst[r] > 0u) {
      flagged = true_v;
      editor_timeline_append(
          legend, sizeof(legend), &length,
          editor_timeline_system_word(r, timeline->system_worst[r]));
    }
  }
  /* Empty bands need words too: the view had nothing to flag. */
  if (!flagged) {
    editor_timeline_append(legend, sizeof(legend), &length,
                           "Thermal, memory, power and errors nominal");
  }
  VkrUiWidgetConfig label =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  label.placement = editor_timeline_cell(0u, row);
  label.placement.justify = VKR_UI_ALIGN_START;
  label.placement.align = VKR_UI_ALIGN_START;
  label.placement.margin_pt = (VkrUiEdges){2.0f, 0.0f, 0.0f, 6.0f};
  label.icon = VKR_UI_ICON_DOT;
  label.icon_size_pt = 7.0f;
  /* The dot takes the view's strongest band colour; the faint fair and warn
     levels leave it neutral, as warn is a 16 GB Mac's normal state. */
  label.icon_color =
      timeline->system_worst[0] >= 3u || timeline->system_worst[1] >= 2u
          ? theme->error
      : timeline->system_worst[0] >= 2u || timeline->system_worst[2] > 0u
          ? theme->warning
          : theme->text_secondary;
  vkr_ui_label(ui, string8_lit("system.legend"),
               string8_create_formatted(ui->frame_allocator, "%s", legend),
               &label);
}

/* The lane's x, in points from the lane's left edge, of `frame`. */
vkr_internal float32_t
editor_timeline_x(const VkrEditorRecordingTimeline *timeline,
                  float32_t width_pt, uint64_t frame) {
  return width_pt * ((float32_t)(frame - timeline->view_first) + 0.5f) /
         (float32_t)editor_timeline_span(timeline);
}

/* A mark of `size_pt` in lane `row` at `x`: a dot, a note's square, a
   range's span or a column's engine events; a click seeks to `seek_frame`.
   */
vkr_internal bool8_t editor_timeline_mark(VkrEditorUi *editor, VkrUiSystem *ui,
                                          const char *id, uint64_t index,
                                          uint32_t row, Vec2 size_pt,
                                          float32_t x, Vec4 color,
                                          float32_t radius, uint64_t seek_frame,
                                          String8 tooltip) {
  VkrUiWidgetConfig mark = vkr_ui_widget_config_default();
  mark.placement = editor_timeline_cell(0u, row);
  mark.placement.justify = VKR_UI_ALIGN_START;
  mark.placement.margin_pt.left = x;
  mark.style.min_size_pt = mark.style.max_size_pt = size_pt;
  mark.style.background_color = color;
  mark.style.hover_background_color = vkr_ui_theme()->text;
  mark.style.corner_radius_pt = (Vec4){radius, radius, radius, radius};
  mark.tooltip = tooltip;
  (void)vkr_ui_push_id_u64(ui, index);
  const bool8_t clicked = vkr_ui_button(
      ui, string8_create_from_cstr((const uint8_t *)id, string_length(id)),
      (String8){0}, &mark);
  if (clicked) {
    VkrSampleRecordingRequest seek = {
        .command = VKR_SAMPLE_RECORDING_SEEK_FRAME, .frame = seek_frame};
    (void)vkr_editor_recording_send(editor, seek, false_v);
  }
  (void)vkr_ui_pop_id(ui);
  return clicked;
}

/* Segments, then markers, then notes in the view, placed under the
   frame-time lane's last drawn width. */
vkr_internal void editor_timeline_events(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const VkrRecordingPlayback *playback,
                                         float32_t width, uint32_t row) {
  const VkrEditorRecordingTimeline *timeline = &editor->recording.timeline;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t half = EDITOR_TIMELINE_MARK_PT * 0.5f;
  char error[192];
  const VkrRecordingAnnotations *annotations = vkr_editor_recording_annotations(
      editor, playback->bundle_path, error, sizeof(error));
  for (uint32_t i = 0u; annotations && i < annotations->segment_count; ++i) {
    const VkrRecordingSegment *segment = &annotations->segments[i];
    if (segment->last < timeline->view_first ||
        segment->first > timeline->view_last) {
      continue;
    }
    const float32_t x0 = editor_timeline_x(
        timeline, width, Max(segment->first, timeline->view_first));
    const float32_t x1 = editor_timeline_x(
        timeline, width, Min(segment->last, timeline->view_last));
    /* A click also opens the segment's panel. */
    if (editor_timeline_mark(
            editor, ui, "segment", i, row,
            (Vec2){Max(x1 - x0, EDITOR_TIMELINE_MARK_PT),
                   EDITOR_TIMELINE_MARK_PT},
            x0, vkr_ui_color_alpha(theme->accent, 0.45f), 3.0f, segment->first,
            string8_create_formatted(
                ui->frame_allocator, "%s: frames %llu to %llu%s%s",
                segment->name, (unsigned long long)segment->first,
                (unsigned long long)segment->last, segment->note[0] ? "\n" : "",
                segment->note))) {
      vkr_editor_recording_segment_select(editor, i);
    }
  }
  for (uint32_t i = 0u; i < playback->marker_count; ++i) {
    const VkrRecordingMarker *marker = &playback->markers[i];
    if (marker->frame < timeline->view_first ||
        marker->frame > timeline->view_last) {
      continue;
    }
    (void)editor_timeline_mark(
        editor, ui, "marker", i, row,
        (Vec2){EDITOR_TIMELINE_MARK_PT, EDITOR_TIMELINE_MARK_PT},
        editor_timeline_x(timeline, width, marker->frame) - half,
        vkr_editor_recording_marker_color(marker->kind), half, marker->frame,
        string8_create_formatted(ui->frame_allocator, "%s at frame %llu%s%s",
                                 vkr_recording_marker_kind_name(marker->kind),
                                 (unsigned long long)marker->frame,
                                 marker->label[0] ? ": " : "", marker->label));
  }
  for (uint32_t i = 0u; annotations && i < annotations->note_count; ++i) {
    const VkrRecordingNote *note = &annotations->notes[i];
    if (note->frame < timeline->view_first ||
        note->frame > timeline->view_last) {
      continue;
    }
    (void)editor_timeline_mark(
        editor, ui, "note", i, row,
        (Vec2){EDITOR_TIMELINE_MARK_PT, EDITOR_TIMELINE_MARK_PT},
        editor_timeline_x(timeline, width, note->frame) - half, theme->text,
        2.0f, note->frame,
        string8_create_formatted(ui->frame_allocator, "Note at frame %llu:\n%s",
                                 (unsigned long long)note->frame, note->text));
  }
}

/* Engine events the lane names and colours by kind: draws waiting for
   their pipelines, asset loads, world cells and script reloads. A script's own
   event keeps its name. */
typedef struct EditorTimelineEventKind {
  const char *name;
  const char *label;
  Vec4 color;
} EditorTimelineEventKind;

vkr_internal EditorTimelineEventKind
editor_timeline_event_kind(const char *name) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const Vec4 violet = {0.66f, 0.55f, 0.98f, 1.0f};
  const Vec4 teal = {0.30f, 0.78f, 0.80f, 1.0f};
  const EditorTimelineEventKind kinds[] = {
      {VKR_RECORDING_EVENT_PIPELINE, "Pipeline builds", violet},
      {VKR_RECORDING_EVENT_SHADER, "Shader library loads", violet},
      {VKR_RECORDING_EVENT_PIPELINE_LATE, "Draws waiting for pipelines",
       violet},
      {VKR_RECORDING_EVENT_TEXTURE, "Texture loads", theme->accent},
      {VKR_RECORDING_EVENT_MESH, "Mesh loads", theme->accent},
      {VKR_RECORDING_EVENT_MATERIAL, "Material loads", theme->accent},
      {VKR_RECORDING_EVENT_SCENE, "Scene loads", theme->accent},
      {VKR_RECORDING_EVENT_CELL_LOAD, "Cell loads", teal},
      {VKR_RECORDING_EVENT_CELL_UNLOAD, "Cell unloads", teal},
      {VKR_RECORDING_EVENT_SCRIPT_RELOAD, "Script reloads", theme->warning},
  };
  for (uint32_t i = 0u; i < ArrayCount(kinds); ++i) {
    if (string_equals(name, kinds[i].name)) {
      return kinds[i];
    }
  }
  return (EditorTimelineEventKind){name, name, theme->text_secondary};
}

/* The first event at or after `frame`. */
vkr_internal uint32_t editor_timeline_first_event(
    const VkrRecordingPlayback *playback, uint64_t frame) {
  uint32_t low = 0u;
  uint32_t high = playback->event_count;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2u;
    if (playback->events[middle].frame < frame) {
      low = middle + 1u;
    } else {
      high = middle;
    }
  }
  return low;
}

/* One tick per column holding events, coloured by its costliest kind, with
   every kind's count and time in its tooltip; a click seeks to its first
   event. */
vkr_internal void editor_timeline_engine(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const VkrRecordingPlayback *playback,
                                         float32_t width, uint32_t row) {
  const VkrEditorRecordingTimeline *timeline = &editor->recording.timeline;
  VkrUiSystem *ui = frame->ui;
  uint32_t i = editor_timeline_first_event(playback, timeline->view_first);
  while (i < playback->event_count &&
         playback->events[i].frame <= timeline->view_last) {
    const uint32_t column =
        editor_timeline_column_of(timeline, playback->events[i].frame);
    uint32_t counts[VKR_RECORDING_PLAYBACK_EVENT_NAMES] = {0};
    float32_t ms[VKR_RECORDING_PLAYBACK_EVENT_NAMES] = {0};
    const uint64_t first = playback->events[i].frame;
    uint64_t last = first;
    for (; i < playback->event_count &&
           playback->events[i].frame <= timeline->view_last &&
           editor_timeline_column_of(timeline, playback->events[i].frame) ==
               column;
         ++i) {
      const VkrRecordingPlaybackEvent *event = &playback->events[i];
      counts[event->name] += event->count;
      ms[event->name] += event->ms;
      last = event->frame;
    }
    char tooltip[512];
    int32_t length = 0;
    uint32_t heaviest = UINT32_MAX;
    for (uint32_t name = 0u; name < playback->event_name_count; ++name) {
      if (counts[name] == 0u) {
        continue;
      }
      if (heaviest == UINT32_MAX || ms[name] > ms[heaviest] ||
          (ms[name] == ms[heaviest] && counts[name] > counts[heaviest])) {
        heaviest = name;
      }
      const EditorTimelineEventKind kind =
          editor_timeline_event_kind(playback->event_names[name]);
      if (length >= 0 && length < (int32_t)sizeof(tooltip)) {
        length += ms[name] > 0.0f
                      ? string_format(tooltip + length,
                                      sizeof(tooltip) - (uint64_t)length,
                                      "%s: %u  \xc2\xb7  %.1f ms\n", kind.label,
                                      counts[name], ms[name])
                      : string_format(tooltip + length,
                                      sizeof(tooltip) - (uint64_t)length,
                                      "%s: %u\n", kind.label, counts[name]);
      }
    }
    if (heaviest == UINT32_MAX) {
      continue;
    }
    String8 text =
        first == last
            ? string8_create_formatted(ui->frame_allocator, "%sFrame %llu",
                                       tooltip, (unsigned long long)first)
            : string8_create_formatted(
                  ui->frame_allocator, "%sFrames %llu to %llu", tooltip,
                  (unsigned long long)first, (unsigned long long)last);
    const float32_t x =
        editor_timeline_x(timeline, width, (first + last) / 2u) -
        EDITOR_TIMELINE_TICK_WIDTH_PT * 0.5f;
    (void)editor_timeline_mark(
        editor, ui, "engine", column, row,
        (Vec2){EDITOR_TIMELINE_TICK_WIDTH_PT, EDITOR_TIMELINE_ENGINE_PT}, x,
        editor_timeline_event_kind(playback->event_names[heaviest]).color, 1.0f,
        first, text);
  }
}

/* Time labels at a round step that leaves each one room. */
vkr_internal void editor_timeline_ruler(VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame,
                                        const VkrRecordingPlayback *playback,
                                        float32_t width, uint32_t row) {
  const VkrEditorRecordingTimeline *timeline = &editor->recording.timeline;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float64_t start = playback->times[timeline->view_first];
  const float64_t end = playback->times[timeline->view_last];
  const float64_t seconds = Max(end - start, 1.0e-3);
  static const float64_t steps[] = {0.01, 0.02, 0.05,  0.1,   0.2,
                                    0.5,  1.0,  2.0,   5.0,   10.0,
                                    30.0, 60.0, 120.0, 300.0, 600.0};
  const uint32_t labels = (uint32_t)Max(width / EDITOR_TIMELINE_TICK_PT, 1.0f);
  float64_t step = steps[ArrayCount(steps) - 1u];
  for (uint32_t i = 0u; i < ArrayCount(steps); ++i) {
    if (seconds / steps[i] <= (float64_t)labels) {
      step = steps[i];
      break;
    }
  }
  uint32_t index = 0u;
  for (float64_t t = vkr_ceil_f64(start / step) * step; t <= end + 1.0e-9;
       t += step) {
    const uint64_t at = vkr_recording_playback_frame_at(playback, t);
    VkrUiWidgetConfig label =
        vkr_editor_text_config(theme->font_caption, theme->text_secondary);
    label.placement = editor_timeline_cell(0u, row);
    label.placement.justify = VKR_UI_ALIGN_START;
    label.placement.margin_pt.left = editor_timeline_x(timeline, width, at);
    label.icon = VKR_UI_ICON_DOT;
    label.icon_size_pt = 5.0f;
    (void)vkr_ui_push_id_u64(ui, index++);
    vkr_ui_label(ui, string8_lit("tick"),
                 t < 60.0 ? string8_create_formatted(ui->frame_allocator,
                                                     step < 0.1   ? "%.2f s"
                                                     : step < 1.0 ? "%.1f s"
                                                                  : "%.0f s",
                                                     t)
                          : string8_create_formatted(
                                ui->frame_allocator, "%u:%04.1f",
                                (uint32_t)(t / 60.0),
                                t - 60.0 * (float64_t)(uint32_t)(t / 60.0)),
                 &label);
    (void)vkr_ui_pop_id(ui);
  }
}

/* Over the frame-time lane, above the hovered column: the thumbnail of the
   frame under the pointer, with its number, time and frame time. */
vkr_internal void editor_timeline_preview(VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame,
                                          const VkrRecordingPlayback *playback,
                                          Vec3 bar_pt, VkrUiRect lane) {
  VkrEditorRecordingTimeline *timeline = &editor->recording.timeline;
  const uint32_t hovered = timeline->hovered_column;
  timeline->previewed = false_v;
  if (hovered == VKR_UI_PLOT_NONE || hovered >= timeline->column_count) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const uint64_t at = timeline->column_frame[hovered];
  VkrTextureHandle texture = {0};
  Vec2 size = {0};
  bool8_t any = false_v;
  const bool8_t ready = vkr_editor_recording_thumb_preview(
      editor, ui, playback->bundle_path, at, &texture, &size, &any);
  timeline->previewed = any;
  if (!any) {
    return;
  }
  /* The image keeps the thumbnail's aspect at a fixed width; until the
     first one loads, a 16:9 placeholder holds its place. */
  const float32_t image_width = EDITOR_TIMELINE_PREVIEW_PT;
  const float32_t image_height = ready && size.x > 0.0f
                                     ? image_width * size.y / size.x
                                     : image_width * 9.0f / 16.0f;
  const float32_t width = image_width + 8.0f;
  const float32_t height = image_height + 8.0f + 3.0f + 16.0f;
  const float32_t scale = ui->content_scale;
  const float32_t center = (lane.x + lane.width * ((float32_t)hovered + 0.5f) /
                                         (float32_t)timeline->column_count) /
                           scale;
  const float32_t left = vkr_clamp_f32(center - width * 0.5f, bar_pt.x,
                                       bar_pt.x + bar_pt.z - width);
  const float32_t top = bar_pt.y - editor_timeline_panel_height(timeline) -
                        EDITOR_TIMELINE_GAP_PT * 2.0f - height;

  const VkrUiTrack column = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {{.value = image_height, .unit = VKR_UI_TRACK_PX},
                             {.value = 16.0f, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = VKR_UI_PLACEMENT_DEFAULT;
  panel.placement.column = panel.placement.row = 0u;
  panel.placement.justify = panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt = (VkrUiEdges){top, 0.0f, 0.0f, left};
  panel.style = vkr_editor_glass_style();
  panel.style.padding_pt = (VkrUiEdges){4.0f, 4.0f, 4.0f, 4.0f};
  panel.style.gap_pt = 3.0f;
  panel.style.min_size_pt = panel.style.max_size_pt = (Vec2){width, height};
  panel.columns = &column;
  panel.column_count = 1u;
  panel.rows = rows;
  panel.row_count = ArrayCount(rows);
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.recording.timeline.preview"),
                          &panel)) {
    return;
  }
  VkrUiWidgetConfig image = vkr_ui_widget_config_default();
  image.placement = editor_timeline_cell(0u, 0u);
  image.placement.align = VKR_UI_ALIGN_STRETCH;
  image.style.corner_radius_pt = (Vec4){4.0f, 4.0f, 4.0f, 4.0f};
  if (ready) {
    vkr_ui_image(ui, string8_lit("image"),
                 (VkrUiTextureRef){texture.id, texture.generation}, size,
                 &image);
  } else {
    image.style.background_color = vkr_ui_color_alpha(theme->window, 0.8f);
    vkr_ui_label(ui, string8_lit("image"), (String8){0}, &image);
  }
  VkrUiWidgetConfig caption = vkr_ui_widget_config_default();
  caption.placement = editor_timeline_cell(0u, 1u);
  caption.placement.justify = VKR_UI_ALIGN_CENTER;
  caption.style.font_size_pt = theme->font_caption;
  caption.style.text_color = theme->text_secondary;
  vkr_ui_label(ui, string8_lit("caption"),
               string8_create_formatted(
                   ui->frame_allocator,
                   "Frame %llu  \xc2\xb7  %.2f ms  \xc2\xb7  %.2f s",
                   (unsigned long long)at, timeline->column_ms[hovered],
                   playback->times[at]),
               &caption);
  (void)vkr_ui_panel_end(ui);
}

void vkr_editor_recording_timeline_build(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const VkrRecordingPlayback *playback,
                                         Vec3 bar_pt) {
  VkrEditorRecordingTimeline *timeline = &editor->recording.timeline;
  if (!timeline->open || playback->frame_count == 0u) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  editor_timeline_reset(timeline, playback);
  /* A playing playhead pulls the view along. */
  const uint64_t current =
      vkr_recording_playback_frame_at(playback, playback->time);
  const uint64_t span = editor_timeline_span(timeline);
  if (playback->playing &&
      (current < timeline->view_first || current > timeline->view_last)) {
    editor_timeline_view(timeline, playback, (int64_t)current, span);
  }
  editor_timeline_columns(timeline, playback);

  /* The engine-event, memory and System lanes show only for recordings that
     have events, memory samples or platform signals. */
  const bool8_t engine_lane = playback->event_count > 0u;
  timeline->memory_lane = playback->has_memory;
  VkrUiTrack rows[7] = {
      {.value = 24.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
  };
  uint32_t row_count = 2u;
  const uint32_t engine_row = row_count;
  if (engine_lane) {
    rows[row_count++] = (VkrUiTrack){.value = EDITOR_TIMELINE_ENGINE_PT,
                                     .unit = VKR_UI_TRACK_PX};
  }
  const uint32_t memory_row = row_count;
  if (timeline->memory_lane) {
    rows[row_count++] = (VkrUiTrack){.value = EDITOR_TIMELINE_MEMORY_PT,
                                     .unit = VKR_UI_TRACK_PX};
  }
  const uint32_t system_row = row_count;
  if (timeline->system_lane) {
    rows[row_count++] =
        (VkrUiTrack){.value = editor_timeline_system_height(timeline),
                     .unit = VKR_UI_TRACK_PX};
  }
  const uint32_t marks_row = row_count;
  rows[row_count++] = (VkrUiTrack){.value = EDITOR_TIMELINE_MARK_PT + 2.0f,
                                   .unit = VKR_UI_TRACK_PX};
  rows[row_count++] = (VkrUiTrack){.value = 14.0f, .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack column = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = VKR_UI_PLACEMENT_DEFAULT;
  panel.placement.column = panel.placement.row = 0u;
  panel.placement.justify = panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt =
      (VkrUiEdges){bar_pt.y - editor_timeline_panel_height(timeline) -
                       EDITOR_TIMELINE_GAP_PT,
                   0.0f, 0.0f, bar_pt.x};
  panel.style = vkr_editor_glass_style();
  panel.style.padding_pt = (VkrUiEdges){6.0f, 10.0f, 6.0f, 10.0f};
  panel.style.gap_pt = EDITOR_TIMELINE_ROW_GAP_PT;
  panel.style.min_size_pt = panel.style.max_size_pt =
      (Vec2){bar_pt.z, editor_timeline_panel_height(timeline)};
  panel.columns = &column;
  panel.column_count = 1u;
  panel.rows = rows;
  panel.row_count = row_count;
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.recording.timeline"),
                          &panel)) {
    return;
  }
  editor_timeline_header(editor, frame, playback);
  editor_timeline_columns(timeline, playback);
  editor_timeline_plot(editor, frame, playback);
  if (timeline->memory_lane) {
    editor_timeline_memory(editor, frame, playback, memory_row);
  }
  if (timeline->system_lane) {
    editor_timeline_system(editor, frame, playback, system_row);
  }
  /* Marks and labels follow the lane's width from its last drawing. */
  VkrUiRect lane = {0};
  if (vkr_ui_widget_rect(ui, timeline->plot_id, &lane)) {
    const float32_t width = lane.width / ui->content_scale;
    if (engine_lane) {
      editor_timeline_engine(editor, frame, playback, width, engine_row);
    }

    editor_timeline_events(editor, frame, playback, width, marks_row);
    editor_timeline_ruler(editor, frame, playback, width, marks_row + 1u);
  }
  (void)vkr_ui_panel_end(ui);
  editor_timeline_preview(editor, frame, playback, bar_pt, lane);
}
