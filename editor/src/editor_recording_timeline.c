#include "editor_recording.h"

#include "editor_internal.h"
#include "editor_ui.h"
#include "platform/vkr_platform.h"

/* The timeline (docs/proposals/recordings.md, "Player"): a panel over the
   playback bar with the frame time of the frames it shows, the markers,
   notes and ranges on them, a ruler, zoom, and actions on frames brushed
   across the frame-time lane. The bar's scrub still covers the whole
   recording; the timeline looks closer. */

#define EDITOR_TIMELINE_HEIGHT_PT 150.0f
#define EDITOR_TIMELINE_GAP_PT 6.0f
/* The view shows no fewer frames than this. */
#define EDITOR_TIMELINE_MIN_FRAMES 8u
/* One wheel notch zooms by this factor around the pointer. */
#define EDITOR_TIMELINE_ZOOM 0.8f
/* A ruler label wants at least this much room. */
#define EDITOR_TIMELINE_TICK_PT 90.0f
#define EDITOR_TIMELINE_MARK_PT 9.0f

float32_t vkr_editor_recording_timeline_height(VkrEditorUi *editor) {
  return editor->recording.timeline.open
             ? EDITOR_TIMELINE_HEIGHT_PT + EDITOR_TIMELINE_GAP_PT
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

/* Each column's slowest frame over the view, built when the view changes. */
vkr_internal void
editor_timeline_columns(VkrEditorRecordingTimeline *timeline,
                        const VkrRecordingPlayback *playback) {
  if (timeline->column_count > 0u &&
      timeline->columns_first == timeline->view_first &&
      timeline->columns_last == timeline->view_last) {
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

/* Names the selection as a range of the recording's annotations. */
vkr_internal void
editor_timeline_add_range(VkrEditorUi *editor,
                          const VkrRecordingPlayback *playback) {
  const VkrEditorRecordingTimeline *timeline = &editor->recording.timeline;
  const VkrUiTheme *theme = vkr_ui_theme();
  char error[192];
  VkrRecordingAnnotations *annotations = vkr_editor_recording_annotations(
      editor, playback->bundle_path, error, sizeof(error));
  char name[VKR_RECORDING_RANGE_NAME_MAX];
  if (annotations) {
    string_format(name, sizeof(name), "Range %u",
                  annotations->range_count + 1u);
  }
  if (!annotations ||
      !vkr_recording_annotations_add_range(
          annotations, timeline->select_first, timeline->select_last, "user",
          string8_create_from_cstr((const uint8_t *)name,
                                   string_length(name))) ||
      !vkr_editor_recording_annotations_save(editor)) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, theme->warning,
                     annotations ? "The range could not be added." : error);
    return;
  }
  vkr_editor_toast(editor, VKR_UI_ICON_RULER, theme->text,
                   "Added the selection as a range.");
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

/* The selection and its actions: loop, name as a range, re-run, zoom to,
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
  VkrUiWidgetConfig range = editor_timeline_button(
      editor, 2u, VKR_UI_ICON_RULER,
      string8_lit("Name the selection as a range of the recording"));
  if (vkr_ui_button(ui, string8_lit("range"), string8_lit("Range"), &range)) {
    editor_timeline_add_range(editor, playback);
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
  const uint32_t hovered = timeline->hovered_column;
  if (hovered != VKR_UI_PLOT_NONE && hovered < timeline->column_count) {
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

/* The lane's x, in points from the lane's left edge, of `frame`. */
vkr_internal float32_t
editor_timeline_x(const VkrEditorRecordingTimeline *timeline,
                  float32_t width_pt, uint64_t frame) {
  return width_pt * ((float32_t)(frame - timeline->view_first) + 0.5f) /
         (float32_t)editor_timeline_span(timeline);
}

/* A mark in the event lane at `frame`: a dot, a note's square or a range's
   span; a click seeks to its frame. */
vkr_internal void editor_timeline_mark(VkrEditorUi *editor, VkrUiSystem *ui,
                                       const char *id, uint64_t index,
                                       float32_t x, float32_t width, Vec4 color,
                                       float32_t radius, uint64_t seek_frame,
                                       String8 tooltip) {
  VkrUiWidgetConfig mark = vkr_ui_widget_config_default();
  mark.placement = editor_timeline_cell(0u, 2u);
  mark.placement.justify = VKR_UI_ALIGN_START;
  mark.placement.margin_pt.left = x;
  mark.style.min_size_pt = mark.style.max_size_pt =
      (Vec2){width, EDITOR_TIMELINE_MARK_PT};
  mark.style.background_color = color;
  mark.style.hover_background_color = vkr_ui_theme()->text;
  mark.style.corner_radius_pt = (Vec4){radius, radius, radius, radius};
  mark.tooltip = tooltip;
  (void)vkr_ui_push_id_u64(ui, index);
  if (vkr_ui_button(
          ui, string8_create_from_cstr((const uint8_t *)id, string_length(id)),
          (String8){0}, &mark)) {
    VkrSampleRecordingRequest seek = {
        .command = VKR_SAMPLE_RECORDING_SEEK_FRAME, .frame = seek_frame};
    (void)vkr_editor_recording_send(editor, seek, false_v);
  }
  (void)vkr_ui_pop_id(ui);
}

/* Markers, then ranges, then notes in the view, placed under the frame-time
   lane's last drawn width. */
vkr_internal void editor_timeline_events(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const VkrRecordingPlayback *playback,
                                         float32_t width) {
  const VkrEditorRecordingTimeline *timeline = &editor->recording.timeline;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t half = EDITOR_TIMELINE_MARK_PT * 0.5f;
  char error[192];
  const VkrRecordingAnnotations *annotations = vkr_editor_recording_annotations(
      editor, playback->bundle_path, error, sizeof(error));
  for (uint32_t i = 0u; annotations && i < annotations->range_count; ++i) {
    const VkrRecordingRange *range = &annotations->ranges[i];
    if (range->last < timeline->view_first ||
        range->first > timeline->view_last) {
      continue;
    }
    const float32_t x0 = editor_timeline_x(
        timeline, width, Max(range->first, timeline->view_first));
    const float32_t x1 = editor_timeline_x(
        timeline, width, Min(range->last, timeline->view_last));
    editor_timeline_mark(
        editor, ui, "range", i, x0, Max(x1 - x0, EDITOR_TIMELINE_MARK_PT),
        vkr_ui_color_alpha(theme->text, 0.3f), 3.0f, range->first,
        string8_create_formatted(ui->frame_allocator, "%s: frames %llu to %llu",
                                 range->name, (unsigned long long)range->first,
                                 (unsigned long long)range->last));
  }
  for (uint32_t i = 0u; i < playback->marker_count; ++i) {
    const VkrRecordingMarker *marker = &playback->markers[i];
    if (marker->frame < timeline->view_first ||
        marker->frame > timeline->view_last) {
      continue;
    }
    editor_timeline_mark(
        editor, ui, "marker", i,
        editor_timeline_x(timeline, width, marker->frame) - half,
        EDITOR_TIMELINE_MARK_PT,
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
    editor_timeline_mark(
        editor, ui, "note", i,
        editor_timeline_x(timeline, width, note->frame) - half,
        EDITOR_TIMELINE_MARK_PT, theme->text, 2.0f, note->frame,
        string8_create_formatted(ui->frame_allocator, "Note at frame %llu:\n%s",
                                 (unsigned long long)note->frame, note->text));
  }
}

/* Time labels at a round step that leaves each one room. */
vkr_internal void editor_timeline_ruler(VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame,
                                        const VkrRecordingPlayback *playback,
                                        float32_t width) {
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
    label.placement = editor_timeline_cell(0u, 3u);
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

  const VkrUiTrack column = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.value = 24.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
      {.value = EDITOR_TIMELINE_MARK_PT + 2.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 14.0f, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = VKR_UI_PLACEMENT_DEFAULT;
  panel.placement.column = panel.placement.row = 0u;
  panel.placement.justify = panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt = (VkrUiEdges){
      bar_pt.y - EDITOR_TIMELINE_HEIGHT_PT - EDITOR_TIMELINE_GAP_PT, 0.0f, 0.0f,
      bar_pt.x};
  panel.style = vkr_editor_glass_style();
  panel.style.padding_pt = (VkrUiEdges){6.0f, 10.0f, 6.0f, 10.0f};
  panel.style.gap_pt = 3.0f;
  panel.style.min_size_pt = panel.style.max_size_pt =
      (Vec2){bar_pt.z, EDITOR_TIMELINE_HEIGHT_PT};
  panel.columns = &column;
  panel.column_count = 1u;
  panel.rows = rows;
  panel.row_count = ArrayCount(rows);
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.recording.timeline"),
                          &panel)) {
    return;
  }
  editor_timeline_header(editor, frame, playback);
  editor_timeline_columns(timeline, playback);
  editor_timeline_plot(editor, frame, playback);
  /* Marks and labels follow the lane's width from its last drawing. */
  VkrUiRect lane = {0};
  if (vkr_ui_widget_rect(ui, timeline->plot_id, &lane)) {
    const float32_t width = lane.width / ui->content_scale;
    editor_timeline_events(editor, frame, playback, width);
    editor_timeline_ruler(editor, frame, playback, width);
  }
  (void)vkr_ui_panel_end(ui);
}
