#include "editor_recording.h"

#include "core/ui/vkr_ui_id.h"
#include "core/vkr_json.h"
#include "editor_agent.h"
#include "editor_internal.h"
#include "editor_ops.h"
#include "editor_projects.h"
#include "editor_ui.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_heap.h"
#include "platform/vkr_platform.h"
#include "vkr_depot_zstd.h"

#define EDITOR_RECORDING_BAR_HEIGHT_PT 34.0f
#define EDITOR_RECORDING_BUTTON_PT 26.0f

/* ------------------------------------------------------------------------ */
/* Requests                                                                  */

uint64_t vkr_editor_recording_send(VkrEditorUi *editor,
                                   VkrSampleRecordingRequest request,
                                   bool8_t toast) {
  VkrEditorRecording *recording = &editor->recording;
  if (recording->queue_count == VKR_EDITOR_RECORDING_QUEUE) {
    return 0u;
  }
  request.token = ++recording->next_token;
  recording->queue[recording->queue_count++] = request;
  if (toast && recording->toast_count < VKR_EDITOR_RECORDING_QUEUE) {
    recording->toast_tokens[recording->toast_count++] = request.token;
  }
  return request.token;
}

/* Shows the outcome of a request the editor's own controls sent. */
vkr_internal void
editor_recording_report(VkrEditorUi *editor,
                        const VkrSampleRecordingStatus *status) {
  VkrEditorRecording *recording = &editor->recording;
  if (status->token == recording->outcome_seen) {
    return;
  }
  recording->outcome_seen = status->token;
  for (uint32_t i = 0u; i < recording->toast_count; ++i) {
    if (recording->toast_tokens[i] != status->token) {
      continue;
    }
    recording->toast_tokens[i] =
        recording->toast_tokens[--recording->toast_count];
    if (status->message[0]) {
      const VkrUiTheme *theme = vkr_ui_theme();
      vkr_editor_toast(
          editor, status->ok ? VKR_UI_ICON_RECORD : VKR_UI_ICON_WARNING_FILL,
          status->ok ? theme->text : theme->warning, status->message);
    }
    return;
  }
}

/* Frames after a recording starts before its thumbnail capture, so the
   Scene shows its first frames; and frames to wait for a capture. */
#define EDITOR_RECORDING_THUMB_DELAY 10u
#define EDITOR_RECORDING_THUMB_TIMEOUT 120u
#define EDITOR_RECORDING_THUMB_TOKEN (1ull << 63u)

/* Writes the captured Scene, shrunk to the thumbnail width, as thumb.png. */
vkr_internal bool8_t editor_recording_thumb_write(
    const VkrSampleUiFrame *frame, const VkrCaptureItemResult *item,
    const char *bundle) {
  const Vec4 rect = frame->mapping_valid
                        ? frame->mapping.image_rect_px
                        : vec4_new(0.0f, 0.0f, (float32_t)item->width,
                                   (float32_t)item->height);
  uint32_t width = 0u;
  uint32_t height = 0u;
  const char *error = NULL;
  uint8_t *pixels =
      vkr_editor_capture_rgba(item, rect, &width, &height, &error);
  if (!pixels) {
    return false_v;
  }
  /* Content's preview loader takes images up to 256 pixels a side. */
  uint32_t target = Min(VKR_EDITOR_RECORDING_THUMB_WIDTH, width);
  while (target > 16u &&
         vkr_editor_image_shrink_height(width, height, target) > 240u) {
    target -= 8u;
  }
  const uint32_t thumb_height =
      vkr_editor_image_shrink_height(width, height, target);
  uint8_t *thumb = vkr_heap_alloc((size_t)target * thumb_height * 4u);
  bool8_t written = false_v;
  if (thumb) {
    vkr_editor_image_shrink(pixels, width, height, target, thumb);
    char path[VKR_RECORDING_PATH_MAX];
    string_format(path, sizeof(path), "%s/%s", bundle,
                  VKR_EDITOR_RECORDING_THUMB_FILE);
    written = vkr_editor_png_write(path, thumb, target, thumb_height);
    vkr_heap_free(thumb);
  }
  vkr_heap_free(pixels);
  return written;
}

/* One capture of the Scene shortly after a recording starts becomes its
   thumbnail. An agent capture asked in the same frame wins; this one tries
   again later, at most three times. */
vkr_internal void editor_recording_thumbnail(VkrEditorRecording *recording,
                                             const VkrSampleUiFrame *frame) {
  const VkrSampleRecordingStatus *status = frame->recording;
  if (!status || !status->recording) {
    return;
  }
  if (!string_equals(recording->thumb_bundle, status->recording_path)) {
    string_format(recording->thumb_bundle, sizeof(recording->thumb_bundle),
                  "%s", status->recording_path);
    recording->thumb_token = 0u;
    recording->thumb_wait = 0u;
    recording->thumb_attempts = 0u;
    recording->thumb_done = false_v;
  }
  if (recording->thumb_done) {
    return;
  }
  ++recording->thumb_wait;
  const VkrSampleCaptureReady *ready = frame->capture_ready;
  if (recording->thumb_token) {
    if (ready && ready->token == recording->thumb_token) {
      recording->thumb_done =
          (!ready->failed && ready->item &&
           editor_recording_thumb_write(frame, ready->item,
                                        recording->thumb_bundle)) ||
          ++recording->thumb_attempts >= 3u;
      recording->thumb_token = 0u;
      recording->thumb_wait = 0u;
    } else if (recording->thumb_wait > EDITOR_RECORDING_THUMB_TIMEOUT) {
      recording->thumb_token = 0u;
      recording->thumb_wait = 0u;
      recording->thumb_done = ++recording->thumb_attempts >= 3u;
    }
    return;
  }
  if (recording->thumb_wait >= EDITOR_RECORDING_THUMB_DELAY &&
      frame->capture_request && !frame->capture_request->request &&
      frame->mapping_valid && !frame->scene_rendering_stopped) {
    recording->thumb_token =
        EDITOR_RECORDING_THUMB_TOKEN | ++recording->thumb_serial;
    recording->thumb_wait = 0u;
    *frame->capture_request = (VkrSampleCaptureRequest){
        .request = true_v, .token = recording->thumb_token};
  }
}

void vkr_editor_recording_update(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame) {
  VkrEditorRecording *recording = &editor->recording;
  if (frame->recording) {
    editor_recording_report(editor, frame->recording);
    /* A drive's measurement arrives with its end. */
    const VkrSampleRecordingStatus *status = frame->recording;
    if (recording->was_driving && !status->driving && status->drive.valid &&
        status->message[0]) {
      const VkrUiTheme *theme = vkr_ui_theme();
      const bool8_t diverged = status->drive.divergent_tick != UINT64_MAX;
      vkr_editor_toast(
          editor,
          diverged ? VKR_UI_ICON_WARNING_FILL : VKR_UI_ICON_GAME_CONTROLLER,
          diverged ? theme->warning : theme->text, status->message);
    }
    recording->was_driving = status->driving;
    if (status->playback &&
        status->playback_edits == VKR_SAMPLE_RECORDING_EDITS_UNAVAILABLE) {
      if (!recording->edits_warned) {
        vkr_editor_toast(editor, VKR_UI_ICON_INFO_FILL, vkr_ui_theme()->text,
                         "The scene changed after this recording, so Watch "
                         "shows today's scene, not its edits.");
      }
      recording->edits_warned = true_v;
    } else if (!status->playback) {
      recording->edits_warned = false_v;
    }
  }
  editor_recording_thumbnail(recording, frame);
  if (recording->queue_count == 0u || !frame->recording_request ||
      frame->recording_request->command != VKR_SAMPLE_RECORDING_NONE) {
    return;
  }
  *frame->recording_request = recording->queue[0];
  --recording->queue_count;
  MemCopy(recording->queue, recording->queue + 1u,
          sizeof(recording->queue[0]) * recording->queue_count);
}

/* ------------------------------------------------------------------------ */
/* Paths                                                                     */

bool8_t vkr_editor_recording_directory(VkrEditorUi *editor, char *out,
                                       uint64_t capacity) {
  const char *workspace =
      editor->projects ? vkr_editor_projects_workspace_root(editor->projects)
                       : "";
  if (workspace && workspace[0]) {
    return string_format(out, capacity, "%s/local/recordings", workspace) <
           (int32_t)capacity;
  }
  char directory[256];
  if (!vkr_editor_agent_directory(directory, sizeof(directory))) {
    return false_v;
  }
  return string_format(out, capacity, "%s/recordings", directory) <
         (int32_t)capacity;
}

/* A bundle name: letters, digits, '-', '_' and '.'; others become '-'. */
vkr_internal void editor_recording_name(const char *name, char *out,
                                        uint64_t capacity) {
  if (!name || !name[0]) {
    const VkrTime now = vkr_platform_get_local_time();
    string_format(out, capacity, "rec-%04d%02d%02d-%02d%02d%02d",
                  now.year + 1900, now.month + 1, now.day, now.hours,
                  now.minutes, now.seconds);
    return;
  }
  uint64_t length = 0u;
  for (const char *c = name; *c && length + 1u < capacity; ++c) {
    const char ch = *c;
    const bool8_t keep = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                         (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                         ch == '.';
    out[length++] = keep ? ch : '-';
  }
  out[length] = '\0';
}

vkr_internal bool8_t editor_recording_ends_with(const char *text,
                                                const char *suffix) {
  const uint64_t length = string_length(text);
  const uint64_t suffix_length = string_length(suffix);
  return length >= suffix_length &&
         string_equals(text + length - suffix_length, suffix);
}

bool8_t vkr_editor_recording_resolve(VkrEditorUi *editor, const char *name,
                                     char *out, uint64_t capacity) {
  if (!name || !name[0]) {
    return false_v;
  }
  const bool8_t path = name[0] == '/' || string_contains(name, "/") ||
                       string_contains(name, "\\");
  if (path) {
    return string_format(out, capacity, "%s", name) < (int32_t)capacity;
  }
  char directory[VKR_RECORDING_PATH_MAX];
  if (!vkr_editor_recording_directory(editor, directory, sizeof(directory))) {
    return false_v;
  }
  return string_format(
             out, capacity, "%s/%s%s", directory, name,
             editor_recording_ends_with(name, VKR_RECORDING_BUNDLE_EXTENSION)
                 ? ""
                 : VKR_RECORDING_BUNDLE_EXTENSION) < (int32_t)capacity;
}

uint64_t vkr_editor_recording_start(VkrEditorUi *editor, const char *name,
                                    const char *author, VkrRecordingTier tier,
                                    bool8_t toast, char *error,
                                    uint64_t error_capacity) {
  char directory[VKR_RECORDING_PATH_MAX];
  if (!vkr_editor_recording_directory(editor, directory, sizeof(directory))) {
    string_format(error, error_capacity, "No directory for recordings.");
    return 0u;
  }
  VkrSampleRecordingRequest request = {
      .command = VKR_SAMPLE_RECORDING_START,
      .codec = vkr_depot_zstd_codec(),
      .tier = tier,
  };
  editor_recording_name(name, request.name, sizeof(request.name));
  string_format(request.author, sizeof(request.author), "%s",
                author && author[0] ? author : "user");
  if (string_format(request.path, sizeof(request.path), "%s/%s%s", directory,
                    request.name, VKR_RECORDING_BUNDLE_EXTENSION) >=
      (int32_t)sizeof(request.path)) {
    string_format(error, error_capacity, "The recording path is too long.");
    return 0u;
  }
  const uint64_t token = vkr_editor_recording_send(editor, request, toast);
  if (!token) {
    string_format(error, error_capacity, "Too many recording requests.");
  }
  return token;
}

/* ------------------------------------------------------------------------ */
/* REC indicator                                                             */

/* Top left of the Scene while recording: time, size and losses. A click
   stops the recording. */
vkr_internal void
editor_recording_indicator_build(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame) {
  const VkrSampleRecordingStatus *status = frame->recording;
  if (!status || !status->recording || !frame->mapping_valid) {
    return;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = frame->ui;
  const float32_t scale = ui->content_scale;
  const Vec4 viewport = frame->mapping.panel_rect_px;
  const uint64_t lost =
      status->stats.dropped_records + status->stats.lost_records;
  const uint32_t minutes = (uint32_t)(status->seconds / 60.0);
  const uint32_t seconds = (uint32_t)status->seconds % 60u;
  const float64_t bytes = (float64_t)status->stats.stored_bytes;
  String8 text = string8_create_formatted(
      ui->frame_allocator, "REC %02u:%02u  \xc2\xb7  %.1f %s%s", minutes,
      seconds, bytes < 1048576.0 ? bytes / 1024.0 : bytes / 1048576.0,
      bytes < 1048576.0 ? "KB" : "MB", lost ? "  \xc2\xb7  data lost" : "");
  VkrUiWidgetConfig chip = vkr_ui_widget_config_default();
  chip.placement = VKR_UI_PLACEMENT_DEFAULT;
  chip.placement.column = chip.placement.row = 0u;
  chip.placement.justify = chip.placement.align = VKR_UI_ALIGN_START;
  chip.placement.margin_pt =
      (VkrUiEdges){viewport.y / scale + vkr_editor_scene_top_pt(frame) + 44.0f,
                   0.0f, 0.0f, viewport.x / scale + 10.0f};
  chip.style = vkr_editor_overlay_style();
  chip.style.padding_pt = (VkrUiEdges){4.0f, 10.0f, 4.0f, 8.0f};
  chip.style.font_size_pt = theme->font_caption;
  chip.style.text_color = lost ? theme->warning : theme->text;
  chip.style.hover_background_color = theme->popup;
  /* The dot pulses once a second, as a camera's tally light does. */
  const float32_t clock = (float32_t)vkr_platform_get_absolute_time();
  const float32_t pulse = clock - vkr_floor_f32(clock) < 0.5f ? 1.0f : 0.45f;
  chip.icon = VKR_UI_ICON_RECORD;
  chip.icon_size_pt = 12.0f;
  chip.icon_color = vkr_ui_color_alpha(theme->error, pulse);
  char stop_key[32] = "the Record button";
  char mark_key[32] = "Mark";
  (void)vkr_editor_command_shortcut(CMD_RECORD, stop_key, sizeof(stop_key));
  (void)vkr_editor_command_shortcut(CMD_RECORD_MARK, mark_key,
                                    sizeof(mark_key));
  chip.tooltip = string8_create_formatted(
      ui->frame_allocator,
      "%s\n%llu frames, %u markers\nClick or %s to stop; %s adds a marker",
      status->recording_path, (unsigned long long)status->frames,
      status->markers, stop_key, mark_key);
  if (vkr_ui_button(ui, string8_lit("editor.recording.rec"), text, &chip)) {
    vkr_editor_command_execute(CMD_RECORD, editor, frame);
  }
}

/* ------------------------------------------------------------------------ */
/* Playback bar                                                              */

vkr_internal void editor_recording_transport(VkrEditorUi *editor,
                                             VkrSampleRecordingCommand command,
                                             float64_t time, uint64_t frame,
                                             float32_t speed, bool8_t loop) {
  const VkrSampleRecordingRequest request = {
      .command = command,
      .time = time,
      .frame = frame,
      .speed = speed,
      .loop = loop,
  };
  (void)vkr_editor_recording_send(editor, request, false_v);
}

/* The marker before or after the playhead, or none. */
vkr_internal const VkrRecordingMarker *
editor_recording_marker_near(const VkrRecordingPlayback *playback,
                             bool8_t forward) {
  const VkrRecordingMarker *best = NULL;
  for (uint32_t i = 0u; i < playback->marker_count; ++i) {
    const VkrRecordingMarker *marker = &playback->markers[i];
    if (forward ? marker->time > playback->time + 1.0e-4
                : marker->time < playback->time - 1.0e-4) {
      if (!best ||
          (forward ? marker->time < best->time : marker->time > best->time)) {
        best = marker;
      }
    }
  }
  return best;
}

vkr_internal VkrUiWidgetConfig editor_recording_button(uint32_t column,
                                                       VkrUiIcon icon,
                                                       String8 tooltip) {
  VkrUiWidgetConfig button =
      vkr_editor_icon_button_config(column, 0u, icon, tooltip);
  button.style.min_size_pt = button.style.max_size_pt =
      (Vec2){EDITOR_RECORDING_BUTTON_PT, EDITOR_RECORDING_BUTTON_PT};
  return button;
}

vkr_internal Vec4 editor_recording_marker_color(uint32_t kind) {
  const VkrUiTheme *theme = vkr_ui_theme();
  switch (kind) {
  case VKR_RECORDING_MARKER_BUG:
    return theme->error;
  case VKR_RECORDING_MARKER_STEP:
    return theme->success;
  case VKR_RECORDING_MARKER_EDIT:
    return theme->warning;
  }
  return theme->info;
}

/* Marker ticks over the scrub slider's last rectangle; a click seeks. */
vkr_internal void editor_recording_markers_build(
    VkrEditorUi *editor, const VkrSampleUiFrame *frame,
    const VkrRecordingPlayback *playback, VkrUiRect slider_px) {
  VkrUiSystem *ui = frame->ui;
  const float32_t scale = ui->content_scale;
  if (playback->duration <= 0.0 || slider_px.width <= 0.0f) {
    return;
  }
  for (uint32_t i = 0u; i < playback->marker_count; ++i) {
    const VkrRecordingMarker *marker = &playback->markers[i];
    const float32_t fraction = (float32_t)(marker->time / playback->duration);
    VkrUiWidgetConfig tick = vkr_ui_widget_config_default();
    tick.placement = VKR_UI_PLACEMENT_DEFAULT;
    tick.placement.column = tick.placement.row = 0u;
    tick.placement.justify = tick.placement.align = VKR_UI_ALIGN_START;
    tick.placement.margin_pt =
        (VkrUiEdges){slider_px.y / scale - 9.0f, 0.0f, 0.0f,
                     (slider_px.x + slider_px.width * fraction) / scale - 4.0f};
    tick.style.min_size_pt = tick.style.max_size_pt = (Vec2){8.0f, 8.0f};
    tick.style.background_color = editor_recording_marker_color(marker->kind);
    tick.style.hover_background_color = vkr_ui_theme()->text;
    tick.style.corner_radius_pt = (Vec4){4.0f, 4.0f, 4.0f, 4.0f};
    tick.tooltip = string8_create_formatted(
        ui->frame_allocator, "%s at %.2f s (frame %llu)%s%s",
        vkr_recording_marker_kind_name(marker->kind), marker->time,
        (unsigned long long)marker->frame, marker->label[0] ? ": " : "",
        marker->label);
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("editor.recording.marker"), (String8){0},
                      &tick)) {
      editor_recording_transport(editor, VKR_SAMPLE_RECORDING_SEEK_FRAME, 0.0,
                                 marker->frame, 0.0f, false_v);
    }
    (void)vkr_ui_pop_id(ui);
  }
}

vkr_internal void
editor_recording_bar_controls(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame,
                              const VkrRecordingPlayback *playback) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig close = editor_recording_button(
      0u, VKR_UI_ICON_CLOSE,
      string8_lit("Close the playback and return to the editor camera"));
  if (vkr_ui_button(ui, string8_lit("close"), (String8){0}, &close)) {
    editor_recording_transport(editor, VKR_SAMPLE_RECORDING_CLOSE, 0.0, 0u,
                               0.0f, false_v);
  }
  VkrUiWidgetConfig play = editor_recording_button(
      1u, playback->playing ? VKR_UI_ICON_PAUSE : VKR_UI_ICON_PLAY,
      playback->playing ? string8_lit("Pause") : string8_lit("Play"));
  play.icon_color = playback->playing ? theme->warning : theme->success;
  if (vkr_ui_button(ui, string8_lit("play"), (String8){0}, &play)) {
    editor_recording_transport(editor,
                               playback->playing ? VKR_SAMPLE_RECORDING_PAUSE
                                                 : VKR_SAMPLE_RECORDING_PLAY,
                               0.0, 0u, 0.0f, false_v);
  }
  const uint64_t current =
      vkr_recording_playback_frame_at(playback, playback->time);
  VkrUiWidgetConfig back = editor_recording_button(
      2u, VKR_UI_ICON_CHEVRON_LEFT, string8_lit("Previous frame"));
  if (vkr_ui_button(ui, string8_lit("back"), (String8){0}, &back)) {
    editor_recording_transport(editor, VKR_SAMPLE_RECORDING_SEEK_FRAME, 0.0,
                               current > 0u ? current - 1u : 0u, 0.0f, false_v);
  }
  VkrUiWidgetConfig next = editor_recording_button(
      3u, VKR_UI_ICON_CHEVRON_RIGHT, string8_lit("Next frame"));
  if (vkr_ui_button(ui, string8_lit("next"), (String8){0}, &next)) {
    editor_recording_transport(editor, VKR_SAMPLE_RECORDING_SEEK_FRAME, 0.0,
                               current + 1u, 0.0f, false_v);
  }
  const VkrRecordingMarker *marker =
      editor_recording_marker_near(playback, true_v);
  VkrUiWidgetConfig to_marker = editor_recording_button(
      4u, VKR_UI_ICON_PIN,
      marker ? string8_create_formatted(
                   ui->frame_allocator, "Next marker: %s at %.2f s",
                   marker->label[0] ? marker->label : "unnamed", marker->time)
             : string8_lit("No marker after the playhead"));
  to_marker.disabled = marker == NULL;
  if (vkr_ui_button(ui, string8_lit("marker"), (String8){0}, &to_marker) &&
      marker) {
    editor_recording_transport(editor, VKR_SAMPLE_RECORDING_SEEK_FRAME, 0.0,
                               marker->frame, 0.0f, false_v);
  }

  VkrUiWidgetConfig time =
      vkr_editor_text_config(theme->font_caption, theme->text);
  time.placement.column = 5u;
  time.placement.row = 0u;
  time.placement.justify = VKR_UI_ALIGN_START;
  time.placement.align = VKR_UI_ALIGN_CENTER;
  vkr_ui_label(ui, string8_lit("time"),
               string8_create_formatted(
                   ui->frame_allocator, "%.2f / %.2f s  \xc2\xb7  %llu / %llu",
                   playback->time, playback->duration,
                   (unsigned long long)current,
                   (unsigned long long)(playback->frame_count - 1u)),
               &time);
}

vkr_internal void
editor_recording_bar_scrub(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                           const VkrRecordingPlayback *playback,
                           VkrUiRect *out_slider_px) {
  VkrUiSystem *ui = frame->ui;
  VkrUiWidgetConfig scrub = vkr_ui_widget_config_default();
  scrub.placement = VKR_UI_PLACEMENT_DEFAULT;
  scrub.placement.column = 6u;
  scrub.placement.row = 0u;
  scrub.placement.justify = VKR_UI_ALIGN_STRETCH;
  scrub.placement.align = VKR_UI_ALIGN_CENTER;
  scrub.placement.margin_pt = (VkrUiEdges){0, 8, 0, 8};
  const VkrUiId slider_id =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("scrub"));
  float32_t time = (float32_t)playback->time;
  if (vkr_ui_slider_f32(ui, string8_lit("scrub"), &time, 0.0f,
                        (float32_t)Max(playback->duration, 0.001), &scrub)) {
    editor_recording_transport(editor, VKR_SAMPLE_RECORDING_SEEK_TIME,
                               (float64_t)time, 0u, 0.0f, false_v);
  }
  if (!vkr_ui_widget_rect(ui, slider_id, out_slider_px)) {
    *out_slider_px = (VkrUiRect){0};
  }

  static const float32_t speeds[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
  uint32_t speed_index = 2u;
  for (uint32_t i = 0u; i < ArrayCount(speeds); ++i) {
    if (playback->speed >= speeds[i] - 0.01f) {
      speed_index = i;
    }
  }
  VkrUiWidgetConfig speed = vkr_ui_widget_config_default();
  speed.placement = VKR_UI_PLACEMENT_DEFAULT;
  speed.placement.column = 7u;
  speed.placement.row = 0u;
  speed.placement.justify = speed.placement.align = VKR_UI_ALIGN_CENTER;
  vkr_editor_ghost_style(&speed);
  speed.style.font_size_pt = vkr_ui_theme()->font_caption;
  speed.tooltip = string8_lit("Playback speed; click for the next one");
  if (vkr_ui_button(ui, string8_lit("speed"),
                    string8_create_formatted(ui->frame_allocator, "%g\xc3\x97",
                                             (double)speeds[speed_index]),
                    &speed)) {
    editor_recording_transport(editor, VKR_SAMPLE_RECORDING_SETTINGS, 0.0, 0u,
                               speeds[(speed_index + 1u) % ArrayCount(speeds)],
                               playback->loop);
  }
  VkrUiWidgetConfig loop = editor_recording_button(
      8u, VKR_UI_ICON_REFRESH,
      playback->loop ? string8_lit("Looping; click to play once")
                     : string8_lit("Play once; click to loop"));
  vkr_editor_toggle_style(&loop, playback->loop);
  if (vkr_ui_button(ui, string8_lit("loop"), (String8){0}, &loop)) {
    editor_recording_transport(editor, VKR_SAMPLE_RECORDING_SETTINGS, 0.0, 0u,
                               playback->speed, !playback->loop);
  }
  VkrUiWidgetConfig analyze = editor_recording_button(
      9u, VKR_UI_ICON_CHART_BAR,
      string8_lit("Analyze this recording: statistics, charts and spikes"));
  vkr_editor_toggle_style(
      &analyze, editor->windows[VKR_EDITOR_WINDOW_PERFORMANCE].visible);
  if (vkr_ui_button(ui, string8_lit("analyze"), (String8){0}, &analyze)) {
    char error[160];
    if (!vkr_editor_recording_analyze(editor, playback->bundle_path, error,
                                      sizeof(error))) {
      vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL,
                       vkr_ui_theme()->warning, error);
    }
  }
}

/* Bottom of the Scene in Playback, above the corner chips: close, play,
   frame steps, next marker, the clock, a scrub bar with marker ticks,
   speed and loop. */
vkr_internal void editor_recording_bar_build(VkrEditorUi *editor,
                                             const VkrSampleUiFrame *frame) {
  const VkrRecordingPlayback *playback =
      frame->recording ? frame->recording->playback : NULL;
  if (!playback || !frame->mapping_valid) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const float32_t scale = ui->content_scale;
  const Vec4 viewport = frame->mapping.panel_rect_px;
  /* The orientation gizmo holds the Scene's bottom-left corner. */
  const float32_t left = 104.0f;
  const float32_t width = viewport.z / scale - left - 10.0f;
  if (width < 360.0f) {
    return;
  }
  const VkrUiTrack button = {.value = EDITOR_RECORDING_BUTTON_PT,
                             .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack columns[] = {
      button,
      button,
      button,
      button,
      button,
      {.value = 150.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
      {.value = 44.0f, .unit = VKR_UI_TRACK_PX},
      button,
      button,
  };
  const VkrUiTrack row = {.value = EDITOR_RECORDING_BUTTON_PT,
                          .unit = VKR_UI_TRACK_PX};
  VkrUiPanelConfig bar = vkr_ui_panel_config_default();
  bar.placement = VKR_UI_PLACEMENT_DEFAULT;
  bar.placement.column = bar.placement.row = 0u;
  bar.placement.justify = bar.placement.align = VKR_UI_ALIGN_START;
  bar.placement.margin_pt =
      (VkrUiEdges){(viewport.y + viewport.w) / scale -
                       EDITOR_RECORDING_BAR_HEIGHT_PT - 40.0f,
                   0.0f, 0.0f, viewport.x / scale + left};
  bar.columns = columns;
  bar.column_count = ArrayCount(columns);
  bar.rows = &row;
  bar.row_count = 1u;
  bar.style = vkr_editor_glass_style();
  bar.style.padding_pt = (VkrUiEdges){4.0f, 6.0f, 4.0f, 6.0f};
  bar.style.min_size_pt = bar.style.max_size_pt =
      (Vec2){width, EDITOR_RECORDING_BAR_HEIGHT_PT};
  VkrUiRect slider_px = {0};
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.recording.bar"), &bar)) {
    return;
  }
  editor_recording_bar_controls(editor, frame, playback);
  editor_recording_bar_scrub(editor, frame, playback, &slider_px);
  (void)vkr_ui_panel_end(ui);
  editor_recording_markers_build(editor, frame, playback, slider_px);
}

/* ------------------------------------------------------------------------ */
/* Route                                                                     */

#define EDITOR_RECORDING_PATH_BREAK UINT8_MAX
/* Palette slots: within budget, up to twice it, beyond, and ahead of the
   playhead. */
#define EDITOR_RECORDING_PATH_AHEAD 3u

/* One point per run of frames: the player's position, or the camera's in a
   recording without a player, and the run's worst frame time. Frames between
   two fixed ticks have no player record and keep the last one; frames before
   the first break the route. */
vkr_internal void
editor_recording_path_prepare(VkrEditorRecording *recording,
                              const VkrRecordingPlayback *playback) {
  if (string_equals(recording->path_bundle, playback->bundle_path)) {
    return;
  }
  string_format(recording->path_bundle, sizeof(recording->path_bundle), "%s",
                playback->bundle_path);
  recording->path_player = playback->has_player;
  const uint64_t frames = playback->frame_count;
  const uint64_t step = (frames + VKR_EDITOR_RECORDING_PATH_POINTS - 1u) /
                        VKR_EDITOR_RECORDING_PATH_POINTS;
  const float64_t budget = VKR_RECORDING_ANALYSIS_BUDGET_MS;
  uint32_t count = 0u;
  uint64_t held = UINT64_MAX;
  for (uint64_t first = 0u;
       first < frames && count < VKR_EDITOR_RECORDING_PATH_POINTS;
       first += Max(step, 1u)) {
    const uint64_t end = Min(first + Max(step, 1u), frames);
    float64_t worst_ms = 0.0;
    uint64_t shown = UINT64_MAX;
    for (uint64_t f = first; f < end; ++f) {
      worst_ms = Max(worst_ms, playback->frames[f].display_dt * 1000.0);
      if (playback->has_player && playback->player_valid[f]) {
        held = f;
      }
      if (shown == UINT64_MAX) {
        shown = playback->has_player ? held : f;
      }
    }
    recording->path_frames[count] = first;
    if (shown == UINT64_MAX) {
      recording->path_classes[count++] = EDITOR_RECORDING_PATH_BREAK;
      continue;
    }
    const float64_t *position = playback->has_player
                                    ? &playback->player_positions[shown * 3u]
                                    : playback->cameras[shown].position;
    MemCopy(recording->path_points[count], position, sizeof(float64_t) * 3u);
    recording->path_classes[count++] = worst_ms > 2.0 * budget ? 2u
                                       : worst_ms > budget     ? 1u
                                                               : 0u;
  }
  recording->path_count = count;
}

/* The route's widget over the Scene image; vkr_editor_recording_project
   gives it points once the frame's camera is known. */
vkr_internal void editor_recording_path_build(VkrEditorUi *editor,
                                              const VkrSampleUiFrame *frame) {
  VkrEditorRecording *recording = &editor->recording;
  recording->path_id = VKR_UI_ID_NONE;
  const VkrRecordingPlayback *playback =
      frame->recording ? frame->recording->playback : NULL;
  if (!playback) {
    recording->path_bundle[0] = '\0';
    return;
  }
  if (!frame->mapping_valid) {
    return;
  }
  editor_recording_path_prepare(recording, playback);
  if (recording->path_count < 2u) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const Vec4 image = frame->mapping.image_rect_px;
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement = VKR_UI_PLACEMENT_DEFAULT;
  config.placement.column = config.placement.row = 0;
  config.placement.justify = config.placement.align = VKR_UI_ALIGN_START;
  config.placement.margin_pt = (VkrUiEdges){image.y / ui->content_scale, 0, 0,
                                            image.x / ui->content_scale};
  config.style.min_size_pt = config.style.max_size_pt =
      (Vec2){image.z / ui->content_scale, image.w / ui->content_scale};
  recording->path_id =
      vkr_ui_path(ui, string8_lit("editor.recording.route"), NULL, &config);
}

void vkr_editor_recording_project(VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame) {
  VkrEditorRecording *recording = &editor->recording;
  const VkrRecordingPlayback *playback =
      frame->recording ? frame->recording->playback : NULL;
  if (recording->path_id == VKR_UI_ID_NONE || !playback ||
      !string_equals(recording->path_bundle, playback->bundle_path)) {
    return;
  }
  const VkrScene *level = frame->scene ? frame->scene : frame->world;
  const Vec3 origin = level ? level->origin_offset : vec3_zero();
  const Vec4 image = frame->mapping.image_rect_px;
  const float32_t scale = frame->ui->content_scale;
  const uint64_t playhead =
      vkr_recording_playback_frame_at(playback, playback->time);
  Vec2 *points = vkr_allocator_alloc(frame->ui->frame_allocator,
                                     sizeof(Vec2) * recording->path_count,
                                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint8_t *colors =
      vkr_allocator_alloc(frame->ui->frame_allocator, recording->path_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!points || !colors) {
    return;
  }
  for (uint32_t i = 0u; i < recording->path_count; ++i) {
    const uint8_t class = recording->path_classes[i];
    colors[i] = recording->path_frames[i] > playhead
                    ? EDITOR_RECORDING_PATH_AHEAD
                    : (class == EDITOR_RECORDING_PATH_BREAK ? 0u : class);
    points[i] = (Vec2){VKR_FLOAT_INFINITY, VKR_FLOAT_INFINITY};
    if (class == EDITOR_RECORDING_PATH_BREAK) {
      continue;
    }
    const float64_t *world = recording->path_points[i];
    const Vec4 clip = mat4_mul_vec4(
        frame->view_projection,
        (Vec4){(float32_t)(world[0] - (float64_t)origin.x),
               (float32_t)(world[1] - (float64_t)origin.y),
               (float32_t)(world[2] - (float64_t)origin.z), 1.0f});
    /* A point behind the camera breaks the route there. */
    if (!vkr_is_finite_f32(clip.w) || clip.w <= 1.0e-3f) {
      continue;
    }
    points[i] = (Vec2){image.z * (clip.x / clip.w * 0.5f + 0.5f) / scale,
                       image.w * (clip.y / clip.w * 0.5f + 0.5f) / scale};
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  const Vec4 palette[] = {theme->success, theme->warning, theme->error,
                          vkr_ui_color_alpha(theme->text, 0.35f)};
  const VkrUiPathConfig path = {
      .points = points,
      .colors = colors,
      .count = recording->path_count,
      .palette = palette,
      .palette_count = ArrayCount(palette),
      .width_pt = 3.0f,
  };
  (void)vkr_ui_path_set(frame->ui, recording->path_id, &path);
}

void vkr_editor_recording_overlays_build(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame) {
  editor_recording_path_build(editor, frame);
  editor_recording_indicator_build(editor, frame);
  editor_recording_bar_build(editor, frame);
}

/* ------------------------------------------------------------------------ */
/* Recordings window                                                         */

vkr_internal void editor_recording_text(VkrJsonReader *reader, const char *key,
                                        char *out, uint64_t capacity) {
  String8 value = {0};
  vkr_json_reader_reset(reader);
  if (vkr_json_get_string(reader, key, &value)) {
    string_format(out, capacity, "%.*s", (int)value.length,
                  (const char *)value.str);
  }
}

vkr_internal float64_t editor_recording_number(VkrJsonReader *reader,
                                               const char *key) {
  float64_t value = 0.0;
  vkr_json_reader_reset(reader);
  return vkr_json_get_double(reader, key, &value) ? value : 0.0;
}

/* Fills an entry from <bundle>/summary.json; a bundle without one (a crash
   before stop) lists by name with an unknown length. */
vkr_internal void editor_recording_entry_read(const char *directory,
                                              VkrEditorRecordingEntry *entry) {
  char path[VKR_RECORDING_PATH_MAX];
  string_format(path, sizeof(path), "%s/%s/%s", directory, entry->bundle,
                VKR_RECORDING_SUMMARY_FILE);
  string_format(entry->name, sizeof(entry->name), "%s", entry->bundle);
  string_format(entry->status, sizeof(entry->status), "no summary");
  FileMapping mapping = {0};
  const FilePath file = file_path_view(path);
  if (file_map_readonly(&file, &mapping) != FILE_ERROR_NONE) {
    return;
  }
  VkrJsonReader reader = vkr_json_reader_create(mapping.data, mapping.size);
  entry->has_summary = true_v;
  editor_recording_text(&reader, "name", entry->name, sizeof(entry->name));
  editor_recording_text(&reader, "scene", entry->scene, sizeof(entry->scene));
  editor_recording_text(&reader, "author", entry->author,
                        sizeof(entry->author));
  editor_recording_text(&reader, "status", entry->status,
                        sizeof(entry->status));
  entry->frames = (uint64_t)editor_recording_number(&reader, "frames");
  entry->duration = editor_recording_number(&reader, "duration_seconds");
  entry->lost = (uint64_t)(editor_recording_number(&reader, "dropped_records") +
                           editor_recording_number(&reader, "lost_records"));
  entry->started_unix =
      (int64_t)editor_recording_number(&reader, "started_unix");
  entry->drivable = editor_recording_number(&reader, "actions") > 0.0 &&
                    editor_recording_number(&reader, "player_ticks") > 0.0;
  /* Markers are an array of objects, each with one "kind". */
  vkr_json_reader_reset(&reader);
  if (vkr_json_find_array(&reader, "markers")) {
    while (vkr_json_next_array_element(&reader) &&
           vkr_json_find_field(&reader, "kind")) {
      ++entry->markers;
    }
  }
  file_unmap(&mapping);
}

typedef struct EditorRecordingListing {
  VkrEditorRecording *recording;
} EditorRecordingListing;

vkr_internal bool8_t editor_recording_list_visit(void *context,
                                                 const char *name,
                                                 bool8_t is_directory) {
  EditorRecordingListing *listing = context;
  VkrEditorRecording *recording = listing->recording;
  if (!is_directory ||
      !editor_recording_ends_with(name, VKR_RECORDING_BUNDLE_EXTENSION) ||
      recording->entry_count == VKR_EDITOR_RECORDING_LIST_MAX) {
    return true_v;
  }
  VkrEditorRecordingEntry *entry =
      &recording->entries[recording->entry_count++];
  MemZero(entry, sizeof(*entry));
  string_format(entry->bundle, sizeof(entry->bundle), "%s", name);
  editor_recording_entry_read(recording->directory, entry);
  return true_v;
}

/* The texture request path of an entry's thumbnail. */
vkr_internal String8 editor_recording_thumb_path(
    const VkrEditorRecording *recording, const VkrEditorRecordingEntry *entry,
    char *out, uint64_t capacity) {
  const int32_t length = string_format(
      out, capacity, "%s/%s/%s?cs=srgb&source=only", recording->directory,
      entry->bundle, VKR_EDITOR_RECORDING_THUMB_FILE);
  return string8_create_from_cstr((const uint8_t *)out,
                                  length > 0 ? (uint64_t)length : 0u);
}

vkr_internal void
editor_recording_release_thumbs(VkrEditorRecording *recording) {
  for (uint32_t i = 0u; i < recording->entry_count; ++i) {
    VkrEditorRecordingEntry *entry = &recording->entries[i];
    if (entry->thumb_request.request_id) {
      char path[VKR_RECORDING_PATH_MAX + 64u];
      vkr_resource_system_unload(
          &entry->thumb_request,
          editor_recording_thumb_path(recording, entry, path, sizeof(path)));
    }
    entry->thumb_request = (VkrResourceHandleInfo){0};
    entry->thumb_texture = (VkrTextureHandle){0};
  }
}

void vkr_editor_recording_shutdown(VkrEditorUi *editor) {
  editor_recording_release_thumbs(&editor->recording);
  vkr_editor_recording_perf_release(&editor->recording.perf);
  editor->recording.entry_count = 0u;
  editor->recording.listed = false_v;
}

/* Width and height from a PNG's header; false for anything else or a side
   past the 256 pixels the preview loader takes. */
vkr_internal bool8_t editor_recording_png_size(const char *path,
                                               uint32_t *width,
                                               uint32_t *height) {
  static const uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  FileMapping mapping = {0};
  const FilePath file = file_path_view(path);
  if (file_map_readonly(&file, &mapping) != FILE_ERROR_NONE) {
    return false_v;
  }
  const bool8_t valid = mapping.size >= 24u &&
                        MemCompare(mapping.data, signature, 8u) == 0 &&
                        MemCompare(mapping.data + 12u, "IHDR", 4u) == 0;
  if (valid) {
    const uint8_t *h = mapping.data;
    *width = (uint32_t)h[16] << 24 | (uint32_t)h[17] << 16 |
             (uint32_t)h[18] << 8 | h[19];
    *height = (uint32_t)h[20] << 24 | (uint32_t)h[21] << 16 |
              (uint32_t)h[22] << 8 | h[23];
  }
  file_unmap(&mapping);
  return valid && *width > 0u && *height > 0u && *width <= 256u &&
         *height <= 256u;
}

/* Starts or polls an entry's thumbnail texture; at most one new load per
   frame, so opening the window never stalls on decoding. */
vkr_internal void editor_recording_thumb_load(VkrEditorRecording *recording,
                                              VkrEditorRecordingEntry *entry,
                                              VkrUiSystem *ui,
                                              bool8_t *load_budget) {
  if (entry->thumb_failed || entry->thumb_texture.id) {
    return;
  }
  if (entry->thumb_request.request_id) {
    VkrResourceHandleInfo resolved = {0};
    if (vkr_resource_system_try_get_resolved(&entry->thumb_request,
                                             &resolved)) {
      entry->thumb_texture = resolved.as.texture;
      return;
    }
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    const VkrResourceLoadState state =
        vkr_resource_system_get_state(&entry->thumb_request, &error);
    entry->thumb_failed = state == VKR_RESOURCE_LOAD_STATE_FAILED ||
                          state == VKR_RESOURCE_LOAD_STATE_CANCELED;
    return;
  }
  if (!*load_budget) {
    return;
  }
  char file[VKR_RECORDING_PATH_MAX];
  string_format(file, sizeof(file), "%s/%s/%s", recording->directory,
                entry->bundle, VKR_EDITOR_RECORDING_THUMB_FILE);
  if (!editor_recording_png_size(file, &entry->thumb_width,
                                 &entry->thumb_height)) {
    entry->thumb_failed = true_v;
    return;
  }
  *load_budget = false_v;
  char path[VKR_RECORDING_PATH_MAX + 64u];
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  entry->thumb_failed = !vkr_resource_system_load(
      VKR_RESOURCE_TYPE_TEXTURE,
      editor_recording_thumb_path(recording, entry, path, sizeof(path)),
      ui->frame_allocator, &entry->thumb_request, &error);
}

vkr_internal void editor_recording_list(VkrEditorUi *editor) {
  VkrEditorRecording *recording = &editor->recording;
  editor_recording_release_thumbs(recording);
  recording->entry_count = 0u;
  recording->delete_armed = 0u;
  recording->listed = true_v;
  if (!vkr_editor_recording_directory(editor, recording->directory,
                                      sizeof(recording->directory))) {
    recording->directory[0] = '\0';
    return;
  }
  EditorRecordingListing listing = {.recording = recording};
  const FilePath path = file_path_view(recording->directory);
  (void)file_list_directory(&path, editor_recording_list_visit, &listing);
  /* Newest first: insertion sort on the start time. */
  for (uint32_t i = 1u; i < recording->entry_count; ++i) {
    const VkrEditorRecordingEntry entry = recording->entries[i];
    uint32_t j = i;
    while (j > 0u &&
           recording->entries[j - 1u].started_unix < entry.started_unix) {
      recording->entries[j] = recording->entries[j - 1u];
      --j;
    }
    recording->entries[j] = entry;
  }
}

/* Removes the bundle's files, then the bundle. */
vkr_internal bool8_t editor_recording_delete_visit(void *context,
                                                   const char *name,
                                                   bool8_t is_directory) {
  const char *bundle = context;
  if (is_directory) {
    return true_v;
  }
  char path[VKR_RECORDING_PATH_MAX];
  string_format(path, sizeof(path), "%s/%s", bundle, name);
  const FilePath file = file_path_view(path);
  (void)file_remove(&file);
  return true_v;
}

vkr_internal bool8_t editor_recording_delete(const char *directory,
                                             const char *bundle_name) {
  char bundle[VKR_RECORDING_PATH_MAX];
  if (!bundle_name[0] ||
      !editor_recording_ends_with(bundle_name,
                                  VKR_RECORDING_BUNDLE_EXTENSION) ||
      string_format(bundle, sizeof(bundle), "%s/%s", directory, bundle_name) >=
          (int32_t)sizeof(bundle)) {
    return false_v;
  }
  const FilePath path = file_path_view(bundle);
  (void)file_list_directory(&path, editor_recording_delete_visit, bundle);
  return file_remove_directory(&path) == FILE_ERROR_NONE;
}

/* A short prompt for an agent: where the bundle is and what to read. */
vkr_internal void
editor_recording_copy_prompt(const char *directory,
                             const VkrEditorRecordingEntry *entry) {
  char prompt[1536];
  const int32_t length = string_format(
      prompt, sizeof(prompt),
      "Review the VKR recording %s/%s (%s, scene %s, %.1f s, %llu frames, "
      "%u markers). Read its summary.json first, then open it with "
      "rec.open and seek its markers with rec.seek, or run "
      "`vkr_rec summary` on it.",
      directory, entry->bundle, entry->name,
      entry->scene[0] ? entry->scene : "unknown", entry->duration,
      (unsigned long long)entry->frames, entry->markers);
  if (length > 0) {
    (void)vkr_platform_clipboard_write_text(
        (const uint8_t *)prompt,
        (uint32_t)Min((uint32_t)length, (uint32_t)sizeof(prompt) - 1u));
  }
}

vkr_internal VkrUiPlacement editor_recording_cell(uint32_t column,
                                                  uint32_t row) {
  return (VkrUiPlacement){.column = column,
                          .row = row,
                          .column_span = 1u,
                          .row_span = 1u,
                          .justify = VKR_UI_ALIGN_STRETCH,
                          .align = VKR_UI_ALIGN_CENTER};
}

vkr_internal String8
editor_recording_meta(VkrUiSystem *ui, const VkrEditorRecordingEntry *entry) {
  char length[32];
  if (entry->duration < 60.0) {
    string_format(length, sizeof(length), "%.1f s", entry->duration);
  } else {
    string_format(length, sizeof(length), "%u:%02u",
                  (uint32_t)(entry->duration / 60.0),
                  (uint32_t)entry->duration % 60u);
  }
  const char *scene = entry->scene[0] ? entry->scene : "unknown scene";
  /* The scene's file name reads better than its path. */
  for (const char *c = entry->scene; *c; ++c) {
    if (*c == '/' || *c == '\\') {
      scene = c + 1;
    }
  }
  return string8_create_formatted(
      ui->frame_allocator,
      "%s  \xc2\xb7  %llu frames  \xc2\xb7  %u marker%s  \xc2\xb7  %s  "
      "\xc2\xb7  %s%s",
      length, (unsigned long long)entry->frames, entry->markers,
      entry->markers == 1u ? "" : "s", entry->author[0] ? entry->author : "?",
      scene, entry->lost ? "  \xc2\xb7  data lost" : "");
}

/* One card: name and status, the facts line, Watch, Analyze, Drive, Copy
   prompt, Delete. */
vkr_internal void editor_recording_card(VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame,
                                        uint32_t index, bool8_t can_watch,
                                        bool8_t *load_budget) {
  VkrEditorRecording *recording = &editor->recording;
  VkrEditorRecordingEntry *entry = &recording->entries[index];
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  editor_recording_thumb_load(recording, entry, ui, load_budget);
  const VkrUiTrack columns[] = {{.value = 84.0f, .unit = VKR_UI_TRACK_PX},
                                {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                                {.value = 76.0f, .unit = VKR_UI_TRACK_PX},
                                {.value = 30.0f, .unit = VKR_UI_TRACK_PX},
                                {.value = 30.0f, .unit = VKR_UI_TRACK_PX},
                                {.value = 30.0f, .unit = VKR_UI_TRACK_PX},
                                {.value = 30.0f, .unit = VKR_UI_TRACK_PX}};
  const VkrUiTrack rows[] = {{.value = 23.0f, .unit = VKR_UI_TRACK_PX},
                             {.value = 23.0f, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig card = vkr_ui_panel_config_default();
  card.placement = editor_recording_cell(0u, index);
  card.placement.align = VKR_UI_ALIGN_STRETCH;
  card.columns = columns;
  card.column_count = ArrayCount(columns);
  card.rows = rows;
  card.row_count = ArrayCount(rows);
  card.style.padding_pt = (VkrUiEdges){6.0f, 10.0f, 6.0f, 6.0f};
  card.style.gap_pt = theme->space_sm;
  card.style.background_color = theme->raised;
  card.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  card.style.border_color = theme->border;
  card.style.corner_radius_pt =
      (Vec4){theme->radius_large, theme->radius_large, theme->radius_large,
             theme->radius_large};
  card.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("card"), &card)) {
    return;
  }
  /* The thumbnail, or the film icon while it loads or when there is none. */
  if (entry->thumb_texture.id) {
    VkrUiWidgetConfig image = vkr_ui_widget_config_default();
    image.placement = editor_recording_cell(0u, 0u);
    image.placement.row_span = 2u;
    image.placement.align = VKR_UI_ALIGN_STRETCH;
    image.style.corner_radius_pt = (Vec4){4.0f, 4.0f, 4.0f, 4.0f};
    vkr_ui_image(
        ui, string8_lit("thumb"),
        (VkrUiTextureRef){entry->thumb_texture.id,
                          entry->thumb_texture.generation},
        (Vec2){(float32_t)entry->thumb_width, (float32_t)entry->thumb_height},
        &image);
  } else {
    VkrUiWidgetConfig picture = vkr_ui_widget_config_default();
    picture.placement = editor_recording_cell(0u, 0u);
    picture.placement.row_span = 2u;
    picture.placement.justify = VKR_UI_ALIGN_CENTER;
    picture.icon = VKR_UI_ICON_VIDEO;
    picture.icon_size_pt = 22.0f;
    picture.icon_color = theme->text_disabled;
    vkr_ui_label(ui, string8_lit("thumb"), (String8){0}, &picture);
  }
  const bool8_t complete = string_equals(entry->status, "complete");
  VkrUiWidgetConfig name =
      vkr_editor_text_config(theme->font_body, theme->text);
  name.placement = editor_recording_cell(1u, 0u);
  name.placement.justify = VKR_UI_ALIGN_START;
  name.text.font = editor->heading_font;
  if (!complete) {
    name.icon = VKR_UI_ICON_WARNING_FILL;
    name.icon_size_pt = 13.0f;
    name.icon_color = theme->warning;
  }
  name.tooltip = string8_create_formatted(ui->frame_allocator, "%s/%s\n%s",
                                          recording->directory, entry->bundle,
                                          entry->status);
  vkr_ui_label(ui, string8_lit("name"),
               string8_create_from_cstr((const uint8_t *)entry->name,
                                        string_length(entry->name)),
               &name);
  VkrUiWidgetConfig meta = vkr_editor_text_config(
      theme->font_caption,
      entry->lost ? theme->warning : theme->text_secondary);
  meta.placement = editor_recording_cell(1u, 1u);
  meta.placement.justify = VKR_UI_ALIGN_START;
  vkr_ui_label(ui, string8_lit("meta"), editor_recording_meta(ui, entry),
               &meta);

  VkrUiWidgetConfig watch = vkr_ui_widget_config_default();
  watch.placement = editor_recording_cell(2u, 0u);
  watch.placement.row_span = 2u;
  vkr_editor_primary_style(&watch, editor->heading_font);
  watch.icon = VKR_UI_ICON_PLAY;
  watch.icon_size_pt = 12.0f;
  watch.disabled = !can_watch || !entry->has_summary;
  watch.tooltip = can_watch ? string8_lit("Watch it in the Scene")
                            : string8_lit("Stop the simulation or the "
                                          "recording first");
  if (vkr_ui_button(ui, string8_lit("watch"), string8_lit("Watch"), &watch)) {
    VkrSampleRecordingRequest request = {.command = VKR_SAMPLE_RECORDING_OPEN};
    string_format(request.path, sizeof(request.path), "%s/%s",
                  recording->directory, entry->bundle);
    (void)vkr_editor_recording_send(editor, request, true_v);
  }
  VkrUiWidgetConfig analyze = vkr_editor_icon_button_config(
      3u, 0u, VKR_UI_ICON_CHART_BAR,
      string8_lit("Analyze: statistics, charts and spikes"));
  analyze.placement.row_span = 2u;
  analyze.disabled = !entry->has_summary;
  if (vkr_ui_button(ui, string8_lit("analyze"), (String8){0}, &analyze)) {
    char error[160];
    char path[VKR_RECORDING_PATH_MAX];
    string_format(path, sizeof(path), "%s/%s", recording->directory,
                  entry->bundle);
    if (!vkr_editor_recording_analyze(editor, path, error, sizeof(error))) {
      vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, theme->warning, error);
    }
  }
  VkrUiWidgetConfig drive = vkr_editor_icon_button_config(
      4u, 0u, VKR_UI_ICON_GAME_CONTROLLER,
      entry->drivable
          ? string8_lit("Drive: play the recorded actions on this build and "
                        "measure how far the player's path parts")
          : string8_lit("Drive needs a Play recording with player actions"));
  drive.placement.row_span = 2u;
  drive.disabled = !entry->drivable || !can_watch;
  if (vkr_ui_button(ui, string8_lit("drive"), (String8){0}, &drive)) {
    VkrSampleRecordingRequest request = {.command = VKR_SAMPLE_RECORDING_DRIVE};
    string_format(request.path, sizeof(request.path), "%s/%s",
                  recording->directory, entry->bundle);
    (void)vkr_editor_recording_send(editor, request, true_v);
  }
  VkrUiWidgetConfig copy = vkr_editor_icon_button_config(
      5u, 0u, VKR_UI_ICON_COPY,
      string8_lit("Copy a prompt that points an agent at this recording"));
  copy.placement.row_span = 2u;
  if (vkr_ui_button(ui, string8_lit("copy"), (String8){0}, &copy)) {
    editor_recording_copy_prompt(recording->directory, entry);
    vkr_editor_toast(editor, VKR_UI_ICON_COPY, theme->text,
                     "Copied an agent prompt for this recording.");
  }
  const bool8_t armed = recording->delete_armed == index + 1u;
  VkrUiWidgetConfig remove = vkr_editor_icon_button_config(
      6u, 0u, VKR_UI_ICON_TRASH,
      armed ? string8_lit("Click again to delete it from disk")
            : string8_lit("Delete this recording"));
  remove.placement.row_span = 2u;
  remove.icon_color = armed ? theme->error : theme->text_secondary;
  if (vkr_ui_button(ui, string8_lit("delete"), (String8){0}, &remove)) {
    if (armed) {
      if (!editor_recording_delete(recording->directory, entry->bundle)) {
        vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, theme->warning,
                         "The recording could not be deleted.");
      }
      recording->listed = false_v;
    } else {
      recording->delete_armed = index + 1u;
    }
  }
  (void)vkr_ui_panel_end(ui);
}

vkr_internal void
editor_recording_window_header(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  VkrEditorRecording *recording = &editor->recording;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack columns[] = {{.value = 1.0f, .unit = VKR_UI_TRACK_FR},
                                {.value = 30.0f, .unit = VKR_UI_TRACK_PX},
                                {.value = 110.0f, .unit = VKR_UI_TRACK_PX}};
  const VkrUiTrack row = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig header = vkr_ui_panel_config_default();
  header.placement = editor_recording_cell(0u, 0u);
  header.columns = columns;
  header.column_count = ArrayCount(columns);
  header.rows = &row;
  header.row_count = 1u;
  header.style.gap_pt = theme->space_sm;
  if (!vkr_ui_panel_begin(ui, string8_lit("header"), &header)) {
    return;
  }
  VkrUiWidgetConfig where =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  where.placement = editor_recording_cell(0u, 0u);
  where.placement.justify = VKR_UI_ALIGN_START;
  where.tooltip =
      string8_create_from_cstr((const uint8_t *)recording->directory,
                               string_length(recording->directory));
  vkr_ui_label(
      ui, string8_lit("count"),
      string8_create_formatted(ui->frame_allocator, "%u recording%s",
                               recording->entry_count,
                               recording->entry_count == 1u ? "" : "s"),
      &where);
  VkrUiWidgetConfig refresh = vkr_editor_icon_button_config(
      1u, 0u, VKR_UI_ICON_REFRESH, string8_lit("Read the directory again"));
  if (vkr_ui_button(ui, string8_lit("refresh"), (String8){0}, &refresh)) {
    recording->listed = false_v;
  }
  const bool8_t active = frame->recording && frame->recording->recording;
  VkrUiWidgetConfig record = vkr_ui_widget_config_default();
  record.placement = editor_recording_cell(2u, 0u);
  vkr_editor_action_style(&record, editor->heading_font);
  record.icon = VKR_UI_ICON_RECORD;
  record.icon_size_pt = 12.0f;
  record.icon_color = theme->error;
  record.disabled = !vkr_editor_command_enabled(CMD_RECORD, editor, frame);
  record.tooltip = vkr_editor_command_tooltip(ui, CMD_RECORD, NULL);
  if (vkr_ui_button(ui, string8_lit("record"),
                    active ? string8_lit("Stop") : string8_lit("Record"),
                    &record)) {
    vkr_editor_command_execute(CMD_RECORD, editor, frame);
  }
  (void)vkr_ui_panel_end(ui);
}

void vkr_editor_recording_window_build(VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       VkrUiRect bounds) {
  VkrEditorRecording *recording = &editor->recording;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const bool8_t active = frame->recording && frame->recording->recording;
  /* A recording that just stopped joins the list. */
  if (!recording->listed || (recording->was_recording && !active)) {
    editor_recording_list(editor);
  }
  recording->was_recording = active;

  const VkrUiTrack column = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {{.value = 30.0f, .unit = VKR_UI_TRACK_PX},
                             {.value = 1.0f, .unit = VKR_UI_TRACK_FR}};
  VkrUiPanelConfig body = vkr_ui_panel_config_default();
  body.placement = editor_recording_cell(0u, 0u);
  body.placement.align = VKR_UI_ALIGN_STRETCH;
  body.columns = &column;
  body.column_count = 1u;
  body.rows = rows;
  body.row_count = ArrayCount(rows);
  body.style.padding_pt = (VkrUiEdges){10.0f, 12.0f, 10.0f, 12.0f};
  body.style.gap_pt = theme->space_md;
  body.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("recordings.body"), &body)) {
    return;
  }
  editor_recording_window_header(editor, frame);
  if (recording->entry_count == 0u) {
    VkrUiWidgetConfig empty =
        vkr_editor_text_config(theme->font_body, theme->text_secondary);
    empty.placement = editor_recording_cell(0u, 1u);
    empty.placement.align = VKR_UI_ALIGN_START;
    empty.text.layout.word_wrap = true_v;
    char record_key[32] = "the Record button";
    (void)vkr_editor_command_shortcut(CMD_RECORD, record_key,
                                      sizeof(record_key));
    vkr_ui_label(ui, string8_lit("empty"),
                 string8_create_formatted(
                     ui->frame_allocator,
                     "No recordings yet. Press %s or the Record button to "
                     "record the camera; stop it the same way. Recordings "
                     "go to %s.",
                     record_key, recording->directory),
                 &empty);
    (void)vkr_ui_panel_end(ui);
    return;
  }
  VkrUiTrack card_rows[VKR_EDITOR_RECORDING_LIST_MAX];
  for (uint32_t i = 0u; i < recording->entry_count; ++i) {
    card_rows[i] = (VkrUiTrack){.value = 64.0f, .unit = VKR_UI_TRACK_PX};
  }
  VkrUiPanelConfig list = vkr_ui_panel_config_default();
  list.placement = editor_recording_cell(0u, 1u);
  list.placement.align = VKR_UI_ALIGN_STRETCH;
  list.columns = &column;
  list.column_count = 1u;
  list.rows = card_rows;
  list.row_count = recording->entry_count;
  list.style.gap_pt = theme->space_sm;
  const bool8_t can_watch =
      !active && !frame->simulation_running && frame->recording_request != NULL;
  (void)bounds;
  bool8_t load_budget = true_v;
  if (vkr_ui_scroll_area_begin(ui, string8_lit("cards"), &list)) {
    for (uint32_t i = 0u; i < recording->entry_count; ++i) {
      (void)vkr_ui_push_id_u64(ui, i);
      editor_recording_card(editor, frame, i, can_watch, &load_budget);
      (void)vkr_ui_pop_id(ui);
    }
    (void)vkr_ui_scroll_area_end(ui);
  }
  (void)vkr_ui_panel_end(ui);
}
