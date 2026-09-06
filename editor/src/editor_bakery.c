#include "editor_bakery.h"
#include "editor_internal.h"

#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"
#include <stdio.h>
#include <string.h>

#define EDITOR_BAKERY_JOB_CAPACITY 16u
#define EDITOR_BAKERY_PATH_CAPACITY 1024u
#define EDITOR_BAKERY_LOG_CAPACITY KB(16)
#define EDITOR_BAKERY_NONE UINT32_MAX

typedef enum EditorBakeKind {
  EDITOR_BAKE_FONT,
  EDITOR_BAKE_TEXTURE,
  EDITOR_BAKE_TEXTURES,
} EditorBakeKind;

typedef enum EditorBakeStatus {
  EDITOR_BAKE_QUEUED,
  EDITOR_BAKE_RUNNING,
  EDITOR_BAKE_SUCCEEDED,
  EDITOR_BAKE_FAILED,
  EDITOR_BAKE_CANCELLED,
} EditorBakeStatus;

typedef struct EditorBakeJob {
  EditorBakeKind kind;
  EditorBakeStatus status;
  char input[EDITOR_BAKERY_PATH_CAPACITY];
  char stdout_path[EDITOR_BAKERY_PATH_CAPACITY];
  char stderr_path[EDITOR_BAKERY_PATH_CAPACITY];
  int32_t exit_code;
  bool8_t force;
  bool8_t process_ok;
  bool8_t timed_out;
} EditorBakeJob;

struct VkrEditorBakery {
  VkrAllocator *allocator;
  VkrThread worker;
  VkrPlatformProcessLock process_lock;
  VkrAtomicBool cancel_requested;
  VkrAtomicBool complete;
  int32_t worker_exit_code;
  bool8_t worker_process_ok;
  bool8_t worker_timed_out;
  EditorBakeJob jobs[EDITOR_BAKERY_JOB_CAPACITY];
  uint32_t job_count;
  uint32_t selected;
  uint32_t running;
  bool8_t force;
  uint8_t texture_path[EDITOR_BAKERY_PATH_CAPACITY];
  uint32_t texture_path_length;
  uint8_t log[EDITOR_BAKERY_LOG_CAPACITY];
  uint32_t log_length;
  float64_t next_log_read;
  char log_directory[EDITOR_BAKERY_PATH_CAPACITY];
  char message[192];
};

static String8 editor_bakery_string(const char *text) {
  return string8_create((uint8_t *)text, strlen(text));
}

static const char *editor_bakery_status(EditorBakeStatus status) {
  static const char *const names[] = {"Queued", "Running", "Done", "Failed",
                                      "Cancelled"};
  return names[status];
}

static bool8_t editor_bakery_directory(const char *name) {
  const FilePath path = {.path = editor_bakery_string(name),
                         .type = FILE_PATH_TYPE_ABSOLUTE};
  return file_create_directory(&path);
}

/* The worker borrows one immutable job and fixed path buffers until complete
 * is released. The UI never edits that job while the worker owns it. */
static bool8_t editor_bakery_is_cancelled(void *context) {
  const VkrEditorBakery *bakery = context;
  return vkr_atomic_bool_load(&bakery->cancel_requested,
                              VKR_MEMORY_ORDER_ACQUIRE);
}

static void *editor_bakery_worker(void *argument) {
  VkrEditorBakery *bakery = argument;
  EditorBakeJob *job = &bakery->jobs[bakery->running];
  const char *arguments[12];
  uint32_t count = 0u;
  char output[EDITOR_BAKERY_PATH_CAPACITY + 5u];
  const char *executable;
  if (job->kind == EDITOR_BAKE_FONT) {
    executable = VKR_EDITOR_FONT_COOKER_PATH;
    arguments[count++] = "--config";
    arguments[count++] = job->input;
  } else {
    executable = VKR_EDITOR_TEXTURE_COOKER_PATH;
    if (job->kind == EDITOR_BAKE_TEXTURES) {
      arguments[count++] = "--input-dir";
      arguments[count++] = job->input;
      arguments[count++] = "--strict";
    } else {
      snprintf(output, sizeof(output), "%s.vkt", job->input);
      arguments[count++] = "--output";
      arguments[count++] = output;
      arguments[count++] = "--type";
      arguments[count++] = "2d";
      arguments[count++] = "--layer";
      arguments[count++] = job->input;
    }
    arguments[count++] = "--basis-threads";
    arguments[count++] = "2";
  }
  if (job->force)
    arguments[count++] = "--force";
  const VkrPlatformProcessConfig config = {
      .executable = executable,
      .arguments = arguments,
      .argument_count = count,
      .working_directory = PROJECT_SOURCE_DIR,
      .stdout_path = job->stdout_path,
      .stderr_path = job->stderr_path,
      .timeout_ms = 30u * 60u * 1000u,
      .termination_grace_ms = 250u,
      .hidden = true_v,
      .is_cancelled = editor_bakery_is_cancelled,
      .cancel_context = bakery,
  };
  bakery->worker_process_ok = vkr_platform_process_run(
      &config, &bakery->worker_exit_code, &bakery->worker_timed_out);
  vkr_atomic_bool_store(&bakery->complete, true_v, VKR_MEMORY_ORDER_RELEASE);
  return NULL;
}

VkrEditorBakery *vkr_editor_bakery_create(VkrAllocator *allocator) {
  if (!allocator || allocator->type != VKR_ALLOCATOR_TYPE_DMEMORY)
    return NULL;
  VkrEditorBakery *bakery = vkr_allocator_alloc(
      allocator, sizeof(*bakery), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!bakery)
    return NULL;
  MemZero(bakery, sizeof(*bakery));
  bakery->allocator = allocator;
  bakery->selected = EDITOR_BAKERY_NONE;
  bakery->running = EDITOR_BAKERY_NONE;
  static const char initial_source[] = "assets/textures/UbuntuMono21px_0.png";
  MemCopy(bakery->texture_path, initial_source, sizeof(initial_source));
  bakery->texture_path_length = sizeof(initial_source) - 1u;
  const int directory_length =
      snprintf(bakery->log_directory, sizeof(bakery->log_directory),
               PROJECT_SOURCE_DIR "build/_artifacts/bakery/%u",
               vkr_platform_get_process_id());
  /* Keep room for each job's /<slot>.stdout.log or .stderr.log suffix. */
  if (directory_length < 0 ||
      directory_length + 20 >= EDITOR_BAKERY_PATH_CAPACITY ||
      !editor_bakery_directory(PROJECT_SOURCE_DIR "build") ||
      !editor_bakery_directory(PROJECT_SOURCE_DIR "build/_artifacts") ||
      !editor_bakery_directory(PROJECT_SOURCE_DIR "build/_artifacts/bakery") ||
      !editor_bakery_directory(bakery->log_directory)) {
    vkr_allocator_free(allocator, bakery, sizeof(*bakery),
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    return NULL;
  }
  return bakery;
}

void vkr_editor_bakery_destroy(VkrEditorBakery *bakery) {
  if (!bakery)
    return;
  if (bakery->worker) {
    vkr_atomic_bool_store(&bakery->cancel_requested, true_v,
                          VKR_MEMORY_ORDER_RELEASE);
    (void)vkr_thread_join(bakery->worker);
    (void)vkr_thread_destroy(bakery->allocator, &bakery->worker);
    vkr_platform_process_lock_release(&bakery->process_lock);
  }
  vkr_allocator_free(bakery->allocator, bakery, sizeof(*bakery),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

static uint32_t editor_bakery_tail(const char *path, uint8_t *bytes,
                                   uint32_t capacity) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return 0u;
  uint32_t length = 0u;
  if (FSEEK64(file, 0, SEEK_END) == 0) {
    const int64_t size = FTELL64(file);
    if (size > 0 &&
        FSEEK64(file, size > capacity ? size - capacity : 0, SEEK_SET) == 0)
      length = (uint32_t)fread(bytes, 1u, capacity, file);
  }
  fclose(file);
  /* A tail may begin inside a multibyte codepoint. */
  uint32_t skip = 0u;
  while (skip < length && (bytes[skip] & 0xc0u) == 0x80u)
    ++skip;
  if (skip > 0u) {
    memmove(bytes, bytes + skip, length - skip);
    length -= skip;
  }
  return length;
}

static void editor_bakery_read_log(VkrEditorBakery *bakery) {
  if (bakery->selected == EDITOR_BAKERY_NONE)
    return;
  const EditorBakeJob *job = &bakery->jobs[bakery->selected];
  uint32_t length =
      (uint32_t)snprintf((char *)bakery->log, sizeof(bakery->log),
                         "%s | exit %d%s\n%s\n\nOutput (tail):\n",
                         editor_bakery_status(job->status), job->exit_code,
                         job->timed_out ? " | timed out" : "", job->input);
  length += editor_bakery_tail(job->stdout_path, bakery->log + length, KB(8));
  static const char errors[] = "\nErrors (tail):\n";
  MemCopy(bakery->log + length, errors, sizeof(errors) - 1u);
  length += sizeof(errors) - 1u;
  length += editor_bakery_tail(job->stderr_path, bakery->log + length,
                               (uint32_t)sizeof(bakery->log) - length - 1u);
  bakery->log[length] = 0u;
  bakery->log_length = length;
}

void vkr_editor_bakery_update(VkrEditorBakery *bakery) {
  if (!bakery)
    return;
  if (bakery->worker &&
      vkr_atomic_bool_load(&bakery->complete, VKR_MEMORY_ORDER_ACQUIRE)) {
    (void)vkr_thread_join(bakery->worker);
    (void)vkr_thread_destroy(bakery->allocator, &bakery->worker);
    vkr_platform_process_lock_release(&bakery->process_lock);
    EditorBakeJob *job = &bakery->jobs[bakery->running];
    job->exit_code = bakery->worker_exit_code;
    job->process_ok = bakery->worker_process_ok;
    job->timed_out = bakery->worker_timed_out;
    /* Completion can race a late Cancel click. A successfully reaped child
       published its output; the request must not relabel that success. */
    job->status = job->process_ok && !job->timed_out && job->exit_code == 0
                      ? EDITOR_BAKE_SUCCEEDED
                  : vkr_atomic_bool_load(&bakery->cancel_requested,
                                         VKR_MEMORY_ORDER_ACQUIRE)
                      ? EDITOR_BAKE_CANCELLED
                      : EDITOR_BAKE_FAILED;
    if (job->status == EDITOR_BAKE_FAILED) {
      log_error("Bakery: %s (exit %d); output: %s", job->input, job->exit_code,
                job->stderr_path);
    } else {
      log_info("Bakery: %s: %s", editor_bakery_status(job->status), job->input);
    }
    bakery->running = EDITOR_BAKERY_NONE;
    bakery->next_log_read = 0.0;
  }
  if (!bakery->worker) {
    for (uint32_t i = 0u; i < bakery->job_count; ++i) {
      EditorBakeJob *job = &bakery->jobs[i];
      if (job->status != EDITOR_BAKE_QUEUED)
        continue;
      bakery->running = i;
      job->status = EDITOR_BAKE_RUNNING;
      bakery->worker_exit_code = -1;
      bakery->worker_process_ok = false_v;
      bakery->worker_timed_out = false_v;
      vkr_atomic_bool_store(&bakery->cancel_requested, false_v,
                            VKR_MEMORY_ORDER_RELAXED);
      vkr_atomic_bool_store(&bakery->complete, false_v,
                            VKR_MEMORY_ORDER_RELAXED);
      if (!vkr_platform_process_lock_acquire(
              "VkrEditorBakery", PROJECT_SOURCE_DIR "build/_artifacts/bakery",
              &bakery->process_lock) ||
          !vkr_thread_create(bakery->allocator, &bakery->worker,
                             editor_bakery_worker, bakery)) {
        vkr_platform_process_lock_release(&bakery->process_lock);
        bakery->running = EDITOR_BAKERY_NONE;
        job->status = EDITOR_BAKE_FAILED;
        snprintf(bakery->message, sizeof(bakery->message),
                 "Unable to start baker; another editor may be baking.");
        log_error("Bakery: unable to launch %s", job->input);
      } else {
        log_info("Bakery: started %s", job->input);
      }
      bakery->next_log_read = 0.0;
      break;
    }
  }
  const float64_t now = vkr_platform_get_absolute_time();
  const bool8_t selected_running =
      bakery->selected < bakery->job_count &&
      bakery->jobs[bakery->selected].status == EDITOR_BAKE_RUNNING;
  if (now >= bakery->next_log_read &&
      (selected_running || bakery->next_log_read == 0.0)) {
    editor_bakery_read_log(bakery);
    bakery->next_log_read = now + 0.25;
  }
}

static void editor_bakery_enqueue(VkrEditorBakery *bakery, EditorBakeKind kind,
                                  const char *input) {
  if (!input[0]) {
    snprintf(bakery->message, sizeof(bakery->message),
             "Enter a source path or font configuration first.");
    return;
  }
  if (bakery->job_count == EDITOR_BAKERY_JOB_CAPACITY) {
    snprintf(bakery->message, sizeof(bakery->message),
             "Queue full; clear finished jobs first.");
    return;
  }
  const uint32_t index = bakery->job_count++;
  EditorBakeJob *job = &bakery->jobs[index];
  *job = (EditorBakeJob){.kind = kind,
                         .status = EDITOR_BAKE_QUEUED,
                         .force = bakery->force,
                         .exit_code = -1};
  snprintf(job->input, sizeof(job->input), "%s", input);
  snprintf(job->stdout_path, sizeof(job->stdout_path), "%s/%u.stdout.log",
           bakery->log_directory, index);
  snprintf(job->stderr_path, sizeof(job->stderr_path), "%s/%u.stderr.log",
           bakery->log_directory, index);
  (void)remove(job->stdout_path);
  (void)remove(job->stderr_path);
  bakery->selected = index;
  bakery->message[0] = '\0';
  bakery->next_log_read = 0.0;
}

static VkrUiWidgetConfig editor_bakery_widget(uint32_t row, uint32_t column) {
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement.row = row;
  config.placement.column = column;
  config.style.padding_pt = (VkrUiEdges){2.0f, 6.0f, 2.0f, 6.0f};
  config.style.font_size_pt = 12.0f;
  return config;
}

void vkr_editor_bakery_build(VkrEditorBakery *bakery, VkrUiSystem *ui,
                             VkrFontHandle heading) {
  if (!bakery)
    return;
  const VkrUiTrack columns[] = {{.unit = VKR_UI_TRACK_FR, .value = 1.0f}};
  /* At the default 175pt panel height, reserve 92pt for controls and share
     the remainder between scrollable jobs and logs. Neither grows the root. */
  const VkrUiTrack rows[] = {
      {.unit = VKR_UI_TRACK_PX, .value = 24.0f},
      {.unit = VKR_UI_TRACK_PX, .value = 24.0f},
      {.unit = VKR_UI_TRACK_PX, .value = 20.0f},
      {.unit = VKR_UI_TRACK_FR, .value = 1.0f},
      {.unit = VKR_UI_TRACK_PX, .value = 24.0f},
      {.unit = VKR_UI_TRACK_FR, .value = 1.0f},
  };
  VkrUiPanelConfig root = vkr_ui_panel_config_default();
  root.columns = columns;
  root.column_count = ArrayCount(columns);
  root.rows = rows;
  root.row_count = ArrayCount(rows);
  root.style.padding_pt = (VkrUiEdges){4.0f, 6.0f, 4.0f, 6.0f};
  if (!vkr_ui_panel_begin(ui, string8_lit("bakery"), &root))
    return;
  const VkrUiTrack actions[] = {
      {.unit = VKR_UI_TRACK_FR, .value = 1.0f},
      {.unit = VKR_UI_TRACK_FR, .value = 1.0f},
      {.unit = VKR_UI_TRACK_FR, .value = 1.0f},
      {.unit = VKR_UI_TRACK_FR, .value = 1.0f},
  };
  VkrUiPanelConfig bar = vkr_ui_panel_config_default();
  bar.columns = actions;
  bar.column_count = ArrayCount(actions);
  bar.placement.row = 0u;
  if (vkr_ui_panel_begin(ui, string8_lit("recipes"), &bar)) {
    VkrUiWidgetConfig config = editor_bakery_widget(0u, 0u);
    vkr_editor_action_style(&config, heading);
    config.tooltip = string8_lit("Bake the regular editor font");
    if (vkr_ui_button(ui, string8_lit("regular"), string8_lit("Regular"),
                      &config))
      editor_bakery_enqueue(bakery, EDITOR_BAKE_FONT,
                            "assets/fonts/UbuntuMono-cooked.fontcfg");
    config.placement.column = 1u;
    config.tooltip = string8_lit("Bake the bold editor font");
    if (vkr_ui_button(ui, string8_lit("bold"), string8_lit("Bold"), &config))
      editor_bakery_enqueue(bakery, EDITOR_BAKE_FONT,
                            "assets/fonts/UbuntuMono-Bold-cooked.fontcfg");
    config.placement.column = 2u;
    config.tooltip = string8_lit("Bake all textures in assets/textures");
    if (vkr_ui_button(ui, string8_lit("textures"), string8_lit("Textures"),
                      &config))
      editor_bakery_enqueue(bakery, EDITOR_BAKE_TEXTURES, "assets/textures");
    config = editor_bakery_widget(0u, 3u);
    config.tooltip = string8_lit("Rebuild outputs even when already current");
    (void)vkr_ui_checkbox(ui, string8_lit("force"), string8_lit("Force"),
                          &bakery->force, &config);
    (void)vkr_ui_panel_end(ui);
  }
  bar.placement.row = 1u;
  if (vkr_ui_panel_begin(ui, string8_lit("source"), &bar)) {
    VkrUiWidgetConfig config = editor_bakery_widget(0u, 0u);
    vkr_editor_field_style(&config);
    config.placement.column_span = 2u;
    config.tooltip = string8_lit(
        "Project-relative or absolute texture/font configuration path");
    VkrUiTextEditBuffer buffer = {.data = bakery->texture_path,
                                  .length = bakery->texture_path_length,
                                  .capacity = sizeof(bakery->texture_path)};
    (void)vkr_ui_text_field(ui, string8_lit("texture.path"), &buffer, &config);
    bakery->texture_path_length = buffer.length;
    vkr_editor_action_style(&config, heading);
    config.placement.column = 2u;
    config.placement.column_span = 1u;
    config.tooltip = string8_lit("Bake the texture at the entered source path");
    if (vkr_ui_button(ui, string8_lit("texture"), string8_lit("Texture"),
                      &config))
      editor_bakery_enqueue(bakery, EDITOR_BAKE_TEXTURE,
                            (char *)bakery->texture_path);
    config.placement.column = 3u;
    config.tooltip = string8_lit("Bake the entered font configuration");
    if (vkr_ui_button(ui, string8_lit("font"), string8_lit("Font"), &config))
      editor_bakery_enqueue(bakery, EDITOR_BAKE_FONT,
                            (char *)bakery->texture_path);
    (void)vkr_ui_panel_end(ui);
  }
  VkrUiWidgetConfig hint = editor_bakery_widget(2u, 0u);
  vkr_ui_label(
      ui, string8_lit("hint"),
      bakery->message[0]
          ? editor_bakery_string(bakery->message)
          : string8_lit("Textures: reload scene. Fonts: restart editor."),
      &hint);
  VkrUiPanelConfig jobs = vkr_ui_panel_config_default();
  jobs.placement.row = 3u;
  VkrUiTrack job_rows[EDITOR_BAKERY_JOB_CAPACITY];
  for (uint32_t i = 0u; i < bakery->job_count; ++i)
    job_rows[i] = (VkrUiTrack){.unit = VKR_UI_TRACK_PX, .value = 22.0f};
  jobs.rows = job_rows;
  jobs.row_count = bakery->job_count;
  if (vkr_ui_scroll_area_begin(ui, string8_lit("jobs"), &jobs)) {
    for (uint32_t i = 0u; i < bakery->job_count; ++i) {
      EditorBakeJob *job = &bakery->jobs[i];
      const String8 text = string8_create_formatted(
          ui->frame_allocator, "%s  %s", editor_bakery_status(job->status),
          job->input);
      VkrUiWidgetConfig config = editor_bakery_widget(i, 0u);
      if (i == bakery->selected)
        config.style.background_color = (Vec4){0.30f, 0.23f, 0.15f, 1.0f};
      if (job->status == EDITOR_BAKE_FAILED)
        config.style.text_color = (Vec4){1.0f, 0.45f, 0.40f, 1.0f};
      else if (job->status == EDITOR_BAKE_SUCCEEDED)
        config.style.text_color = (Vec4){0.55f, 0.88f, 0.66f, 1.0f};
      if (vkr_ui_push_id_u64(ui, i)) {
        if (vkr_ui_button(ui, string8_lit("job"), text, &config)) {
          bakery->selected = i;
          editor_bakery_read_log(bakery);
        }
        (void)vkr_ui_pop_id(ui);
      }
    }
    (void)vkr_ui_scroll_area_end(ui);
  }
  bar.placement.row = 4u;
  if (vkr_ui_panel_begin(ui, string8_lit("job.actions"), &bar)) {
    EditorBakeJob *selected = bakery->selected < bakery->job_count
                                  ? &bakery->jobs[bakery->selected]
                                  : NULL;
    VkrUiWidgetConfig config = editor_bakery_widget(0u, 0u);
    vkr_editor_action_style(&config, heading);
    config.disabled = !selected || (selected->status != EDITOR_BAKE_RUNNING &&
                                    selected->status != EDITOR_BAKE_QUEUED);
    if (vkr_ui_button(ui, string8_lit("cancel"), string8_lit("Cancel"),
                      &config)) {
      if (selected->status == EDITOR_BAKE_RUNNING)
        vkr_atomic_bool_store(&bakery->cancel_requested, true_v,
                              VKR_MEMORY_ORDER_RELEASE);
      else
        selected->status = EDITOR_BAKE_CANCELLED;
      bakery->next_log_read = 0.0;
    }
    config.placement.column = 1u;
    config.disabled = !selected || selected->status < EDITOR_BAKE_SUCCEEDED;
    if (vkr_ui_button(ui, string8_lit("retry"), string8_lit("Retry"),
                      &config)) {
      selected->status = EDITOR_BAKE_QUEUED;
      selected->exit_code = -1;
      selected->timed_out = false_v;
      selected->process_ok = false_v;
      bakery->message[0] = 0;
      (void)remove(selected->stdout_path);
      (void)remove(selected->stderr_path);
      bakery->next_log_read = 0.0;
    }
    config.placement.column = 2u;
    config.disabled = !selected;
    config.tooltip = string8_lit("Copy the selected job log tail");
    if (vkr_ui_button(ui, string8_lit("copy"), string8_lit("Copy"), &config))
      (void)vkr_platform_clipboard_write_text(bakery->log, bakery->log_length);
    config.placement.column = 3u;
    config.disabled = bakery->worker != NULL;
    for (uint32_t i = 0u; i < bakery->job_count; ++i)
      config.disabled |= bakery->jobs[i].status == EDITOR_BAKE_QUEUED;
    config.tooltip = string8_lit("Clear finished jobs once the queue is idle");
    if (vkr_ui_button(ui, string8_lit("clear"), string8_lit("Clear"),
                      &config)) {
      bakery->job_count = 0u;
      bakery->selected = EDITOR_BAKERY_NONE;
      bakery->log_length = 0u;
      bakery->next_log_read = 0.0;
    }
    (void)vkr_ui_panel_end(ui);
  }
  VkrUiPanelConfig logs = vkr_ui_panel_config_default();
  logs.placement.row = 5u;
  const VkrUiTrack log_row = {.unit = VKR_UI_TRACK_AUTO};
  logs.rows = &log_row;
  logs.row_count = 1u;
  if (vkr_ui_scroll_area_begin(ui, string8_lit("logs"), &logs)) {
    VkrUiWidgetConfig config = editor_bakery_widget(0u, 0u);
    uint32_t offset =
        bakery->log_length > KB(4) ? bakery->log_length - KB(4) : 0u;
    while (offset < bakery->log_length &&
           (bakery->log[offset] & 0xc0u) == 0x80u)
      ++offset;
    vkr_ui_label(ui, string8_lit("output"),
                 (String8){.str = bakery->log + offset,
                           .length = bakery->log_length - offset},
                 &config);
    (void)vkr_ui_scroll_area_end(ui);
  }
  (void)vkr_ui_panel_end(ui);
}
