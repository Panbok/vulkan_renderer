#include "editor_recording.h"

#include "assets/vkr_recording_counters.h"
#include "core/vkr_zone.h"
#include "editor_internal.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_heap.h"
#include "platform/vkr_platform.h"

/* GPU counters while recording (ADR-130): Apple GPUs give occupancy and
   their limiters only to Instruments, so the editor traces itself with
   `xctrace record` for a few seconds once a recording starts, with
   signposts on so the trace carries the frame clock, and then imports the
   trace into the recording's bundle. Both run on a thread of their own,
   since the import exports about 47 s per traced second; a chip under the
   recording indicator shows where it is, and a toast its outcome. */

typedef enum EditorCountersPhase {
  EDITOR_COUNTERS_TRACING = 0,
  EDITOR_COUNTERS_IMPORTING,
  EDITOR_COUNTERS_DONE,
} EditorCountersPhase;

struct VkrEditorCounterJob {
  /* Set before the thread starts. */
  uint32_t pid;
  uint32_t seconds;
  char trace[VKR_RECORDING_PATH_MAX];
  char bundle[VKR_RECORDING_PATH_MAX];
  VkrAllocator *allocator;
  VkrThread thread;
  float64_t started;
  /* Shared with the thread. */
  VkrAtomicUint32 phase;
  VkrAtomicBool cancel;
  VkrAtomicUint64 samples;
  /* Written by the thread before it publishes EDITOR_COUNTERS_DONE. */
  bool8_t ok;
  VkrRecordingCountersResult result;
  char error[VKR_RECORDING_COUNTERS_ERROR_MAX];
  /* The main thread's: signposts as they were before the trace. */
  bool8_t signposts_before;
  bool8_t signposts_restored;
};

vkr_internal bool8_t editor_counters_cancelled(void *context) {
  VkrEditorCounterJob *job = context;
  return vkr_atomic_bool_load(&job->cancel, VKR_MEMORY_ORDER_RELAXED);
}

vkr_internal void *editor_counters_main(void *argument) {
  VkrEditorCounterJob *job = argument;
  job->ok = vkr_recording_counters_trace(job->pid, job->seconds, job->trace,
                                         editor_counters_cancelled, job,
                                         job->error, sizeof(job->error));
  if (job->ok) {
    vkr_atomic_uint32_store(&job->phase, EDITOR_COUNTERS_IMPORTING,
                            VKR_MEMORY_ORDER_RELEASE);
    const VkrRecordingCountersImport import = {
        .trace_path = job->trace,
        .bundle_path = job->bundle,
        .is_cancelled = editor_counters_cancelled,
        .cancel_context = job,
        .samples_read = &job->samples,
    };
    job->ok = vkr_recording_counters_import(&import, &job->result);
    if (!job->ok) {
      string_format(job->error, sizeof(job->error), "%s", job->result.error);
    }
  }
  /* The trace is a few hundred megabytes of $TMPDIR. */
  (void)vkr_recording_counters_remove_trace(job->trace);
  vkr_atomic_uint32_store(&job->phase, EDITOR_COUNTERS_DONE,
                          VKR_MEMORY_ORDER_RELEASE);
  return NULL;
}

/* Signposts back as the user had them once the trace no longer needs
   them. */
vkr_internal void editor_counters_restore(VkrEditorCounterJob *job) {
  if (!job->signposts_restored) {
    vkr_zone_signposts_set(job->signposts_before);
    job->signposts_restored = true_v;
  }
}

vkr_internal void editor_counters_free(VkrEditorRecording *recording) {
  VkrEditorCounterJob *job = recording->counter_job;
  if (!job) {
    return;
  }
  (void)vkr_thread_join(job->thread);
  (void)vkr_thread_destroy(job->allocator, &job->thread);
  editor_counters_restore(job);
  vkr_heap_free(job);
  recording->counter_job = NULL;
}

bool8_t vkr_editor_recording_counters_start(VkrEditorUi *editor,
                                            const VkrSampleUiFrame *frame,
                                            uint32_t seconds, char *error,
                                            uint64_t error_capacity) {
  VkrEditorRecording *recording = &editor->recording;
  const VkrSampleRecordingStatus *status = frame->recording;
  if (recording->counter_job) {
    string_format(error, error_capacity,
                  "GPU counters are already being traced or imported.");
    return false_v;
  }
  if (!status || !status->recording ||
      status->recording_rolling_seconds > 0.0 || !status->recording_path[0]) {
    string_format(error, error_capacity,
                  "GPU counters trace a recording in progress.");
    return false_v;
  }
  if (seconds == 0u || seconds > VKR_RECORDING_COUNTERS_TRACE_MAX_S) {
    string_format(error, error_capacity, "A trace lasts 1 to %u s.",
                  VKR_RECORDING_COUNTERS_TRACE_MAX_S);
    return false_v;
  }
  char temp[VKR_RECORDING_PATH_MAX];
  if (!vkr_platform_user_directory(VKR_PLATFORM_USER_TEMP, temp,
                                   sizeof(temp))) {
    string_format(error, error_capacity, "No temporary directory.");
    return false_v;
  }
  VkrEditorCounterJob *job = vkr_heap_alloc_zeroed(1u, sizeof(*job));
  if (!job) {
    string_format(error, error_capacity, "Out of memory.");
    return false_v;
  }
  job->pid = vkr_platform_get_process_id();
  job->seconds = seconds;
  job->started = vkr_platform_get_absolute_time();
  string_format(job->bundle, sizeof(job->bundle), "%s", status->recording_path);
  string_format(temp + string_length(temp), sizeof(temp) - string_length(temp),
                "/vkr");
  const FilePath directory = file_path_view(temp);
  (void)file_create_directories(&directory);
  string_format(job->trace, sizeof(job->trace), "%s/counters-%u-%llu.trace",
                temp, job->pid,
                (unsigned long long)++recording->counter_serial);
  /* The trace places its samples on frames by the frame loop's Clock
     signposts. */
  job->signposts_before = vkr_zone_signposts();
  vkr_zone_signposts_set(true_v);
  job->allocator = &frame->ui->retained_allocator;
  if (!vkr_thread_create(job->allocator, &job->thread, editor_counters_main,
                         job)) {
    editor_counters_restore(job);
    vkr_heap_free(job);
    string_format(error, error_capacity, "The trace thread did not start.");
    return false_v;
  }
  recording->counter_job = job;
  return true_v;
}

void vkr_editor_recording_counters_update(VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame) {
  VkrEditorRecording *recording = &editor->recording;
  const VkrSampleRecordingStatus *status = frame->recording;
  /* A recording that just started traces when asked to. */
  const bool8_t recording_now =
      status && status->recording && status->recording_rolling_seconds <= 0.0;
  if (recording_now && !recording->counters_was_recording &&
      (recording->counters_requested || recording->gpu_counters)) {
    char error[160];
    const uint32_t seconds = recording->counters_requested
                                 ? recording->counters_requested
                                 : VKR_EDITOR_RECORDING_COUNTER_SECONDS;
    if (!vkr_editor_recording_counters_start(editor, frame, seconds, error,
                                             sizeof(error))) {
      vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL,
                       vkr_ui_theme()->warning, error);
    }
  }
  if (recording_now != recording->counters_was_recording) {
    recording->counters_requested = 0u;
  }
  recording->counters_was_recording = recording_now;

  VkrEditorCounterJob *job = recording->counter_job;
  if (!job) {
    return;
  }
  const uint32_t phase =
      vkr_atomic_uint32_load(&job->phase, VKR_MEMORY_ORDER_ACQUIRE);
  if (phase != EDITOR_COUNTERS_TRACING) {
    editor_counters_restore(job);
  }
  if (phase != EDITOR_COUNTERS_DONE) {
    return;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  /* The recording's name, without its directory and extension. */
  const char *name = job->bundle;
  for (const char *at = job->bundle; *at; ++at) {
    name = *at == '/' || *at == '\\' ? at + 1 : name;
  }
  uint64_t name_length = string_length(name);
  const uint64_t extension = string_length(VKR_RECORDING_BUNDLE_EXTENSION);
  if (name_length > extension &&
      string_equals(name + name_length - extension,
                    VKR_RECORDING_BUNDLE_EXTENSION)) {
    name_length -= extension;
  }
  char text[320];
  if (job->ok) {
    string_format(text, sizeof(text),
                  "GPU counters imported into %.*s; its HW tab shows them.",
                  (int32_t)name_length, name);
  } else if (!vkr_atomic_bool_load(&job->cancel, VKR_MEMORY_ORDER_RELAXED)) {
    string_format(text, sizeof(text), "GPU counters for %.*s failed: %s",
                  (int32_t)name_length, name, job->error);
  } else {
    text[0] = '\0';
  }
  if (text[0]) {
    vkr_editor_toast(editor,
                     job->ok ? VKR_UI_ICON_GPU : VKR_UI_ICON_WARNING_FILL,
                     job->ok ? theme->text : theme->warning, text);
  }
  recording->counters_last_ok = job->ok;
  string_format(recording->counters_last, sizeof(recording->counters_last),
                "%s", job->ok ? name : job->error);
  editor_counters_free(recording);
}

void vkr_editor_recording_counters_cancel(VkrEditorUi *editor) {
  VkrEditorCounterJob *job = editor->recording.counter_job;
  if (job) {
    vkr_atomic_bool_store(&job->cancel, true_v, VKR_MEMORY_ORDER_RELAXED);
  }
}

void vkr_editor_recording_counters_shutdown(VkrEditorUi *editor) {
  vkr_editor_recording_counters_cancel(editor);
  editor_counters_free(&editor->recording);
}

bool8_t vkr_editor_recording_counters_state(VkrEditorUi *editor,
                                            VkrEditorCountersState *out) {
  MemZero(out, sizeof(*out));
  const VkrEditorRecording *recording = &editor->recording;
  out->last_ok = recording->counters_last_ok;
  string_format(out->last, sizeof(out->last), "%s", recording->counters_last);
  VkrEditorCounterJob *job = recording->counter_job;
  if (!job) {
    return false_v;
  }
  out->importing =
      vkr_atomic_uint32_load(&job->phase, VKR_MEMORY_ORDER_ACQUIRE) !=
      EDITOR_COUNTERS_TRACING;
  out->seconds = job->seconds;
  out->elapsed = vkr_platform_get_absolute_time() - job->started;
  out->samples =
      vkr_atomic_uint64_load(&job->samples, VKR_MEMORY_ORDER_RELAXED);
  string_format(out->bundle, sizeof(out->bundle), "%s", job->bundle);
  return true_v;
}

void vkr_editor_recording_counters_chip(VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame,
                                        float32_t top_pt) {
  VkrEditorCountersState state;
  if (!vkr_editor_recording_counters_state(editor, &state) ||
      !frame->mapping_valid) {
    return;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = frame->ui;
  const float32_t scale = ui->content_scale;
  const Vec4 viewport = frame->mapping.panel_rect_px;
  VkrUiWidgetConfig chip = vkr_ui_widget_config_default();
  chip.placement = VKR_UI_PLACEMENT_DEFAULT;
  chip.placement.column = chip.placement.row = 0u;
  chip.placement.justify = chip.placement.align = VKR_UI_ALIGN_START;
  chip.placement.margin_pt =
      (VkrUiEdges){top_pt, 0.0f, 0.0f, viewport.x / scale + 10.0f};
  chip.style = vkr_editor_overlay_style();
  chip.style.padding_pt = (VkrUiEdges){4.0f, 10.0f, 4.0f, 8.0f};
  chip.style.font_size_pt = theme->font_caption;
  chip.style.text_color = theme->text_secondary;
  chip.style.hover_background_color = theme->popup;
  chip.icon = state.importing ? VKR_UI_ICON_SPINNER : VKR_UI_ICON_GPU;
  chip.icon_size_pt = 12.0f;
  chip.icon_color = state.importing ? theme->text_secondary : theme->accent;
  const uint32_t elapsed = (uint32_t)state.elapsed;
  String8 text =
      state.importing
          ? string8_create_formatted(
                ui->frame_allocator,
                "GPU counters  \xc2\xb7  importing %.1f M samples  \xc2\xb7  "
                "%u:%02u",
                (float64_t)state.samples / 1.0e6, elapsed / 60u, elapsed % 60u)
          : string8_create_formatted(
                ui->frame_allocator,
                "GPU counters  \xc2\xb7  tracing %u s with Instruments  "
                "\xc2\xb7  %u:%02u",
                state.seconds, elapsed / 60u, elapsed % 60u);
  chip.tooltip = string8_lit(
      "Instruments traces the GPU's counters, then the editor imports them "
      "into the recording: about 47 s per traced second on an M1 Pro, while "
      "you keep working. Click to cancel.");
  if (vkr_ui_button(ui, string8_lit("editor.recording.counters"), text,
                    &chip)) {
    vkr_editor_recording_counters_cancel(editor);
  }
}
