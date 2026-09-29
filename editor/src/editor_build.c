#include "editor_build.h"

#include "editor_internal.h"
#include "editor_projects.h"

#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_hash.h"
#include "core/vkr_json.h"
#include "core/vkr_json_writer.h"
#include "core/vkr_threads.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_file_dialog.h"
#include "platform/vkr_platform.h"
#include "vkr_graphics_settings.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Owns the Build workflow of docs/proposals/project-packaging.md. The
 * package job itself is `vkr_bakery bundle <project> --profile <name>` on
 * Bakery's single worker, so a build never overlaps an import or another
 * workspace write; this file reads its event lines and its report. */

#define BUILD_TAG VKR_ALLOCATOR_MEMORY_TAG_STRUCT
#define BUILD_PATH VKR_EDITOR_PROJECT_PATH_CAPACITY
#define BUILD_STAGE_COUNT 7u
#define BUILD_LOG_CAPACITY KB(16)
#define BUILD_READ_LIMIT MB(1)

#if defined(PLATFORM_APPLE) && defined(__aarch64__)
#define BUILD_HOST_PLATFORM "macos-arm64"
#elif defined(PLATFORM_APPLE)
#define BUILD_HOST_PLATFORM "macos-x64"
#elif defined(PLATFORM_WINDOWS)
#define BUILD_HOST_PLATFORM "windows-x64"
#else
#define BUILD_HOST_PLATFORM "linux-x64"
#endif

/* The package's stages in event order (vkr_bakery_package.c). */
static const char *const build_stage_names[BUILD_STAGE_COUNT] = {
    "Validate",      "Finalize",         "Bake", "Lower", "Pack",
    "Stage runtime", "Verify and report"};

typedef enum BuildPhase {
  BUILD_IDLE = 0,
  BUILD_PREFLIGHT,
  BUILD_RUNNING,
  BUILD_FINISHED,
} BuildPhase;

typedef enum BuildStageStatus {
  BUILD_STAGE_PENDING = 0,
  BUILD_STAGE_RUNNING,
  BUILD_STAGE_DONE,
  BUILD_STAGE_FAILED,
} BuildStageStatus;

typedef struct BuildStage {
  BuildStageStatus status;
  float64_t seconds;
} BuildStage;

struct VkrEditorBuild {
  VkrAllocator *allocator;
  /* The project whose game.json the draft holds. */
  char project_id[37];
  VkrEditorGame draft;
  VkrEditorGame saved;
  char message[256];
  uint32_t profile;
  /* Text-field storage of numeric and list fields. */
  char width_text[8];
  char height_text[8];
  char include_text[1024];
  uint32_t fields_profile;
  bool8_t fields_valid;
  float32_t settings_scroll;
  float32_t panel_scroll;
  /* The build: preflight, then the package job on Bakery's worker. */
  BuildPhase phase;
  bool8_t run_after;
  bool8_t succeeded;
  uint64_t job_id;
  char project_directory[BUILD_PATH];
  char profile_name[64];
  char events_path[BUILD_PATH];
  uint64_t events_offset;
  float64_t next_read;
  BuildStage stages[BUILD_STAGE_COUNT];
  uint32_t stage;
  float64_t fraction;
  char detail[160];
  char report_path[BUILD_PATH];
  char result[256];
  /* The project inventory when the job started; a finalized inventory in the
     report is adopted only while the project still has it. */
  uint8_t assets_digest[VKR_SHA256_DIGEST_SIZE];
  /* The last complete package. */
  char output[BUILD_PATH];
  char executable[BUILD_PATH];
  uint64_t package_bytes;
  uint32_t warning_count;
  uint8_t log[BUILD_LOG_CAPACITY];
  uint32_t log_length;
  /* A game started by Build and Run; its output goes to the Console. */
  VkrThread game;
  VkrAtomicBool game_done;
  VkrAtomicBool game_stop;
  int32_t game_exit;
  char game_path[BUILD_PATH];
  char game_directory[BUILD_PATH];
  char game_log[BUILD_PATH];
  uint64_t game_log_offset;
};

static String8 build_text(const char *text) {
  return string8_create_from_cstr((const uint8_t *)text, strlen(text));
}

static void build_log_line(VkrEditorBuild *build, const char *format, ...) {
  char text[768];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (length <= 0) {
    return;
  }
  if ((uint32_t)length >= sizeof(text)) {
    length = (int)sizeof(text) - 1;
  }
  text[length++] = '\n';
  /* Keep the newest lines: drop whole old lines when the buffer is full. */
  while (build->log_length + (uint32_t)length > BUILD_LOG_CAPACITY &&
         build->log_length) {
    uint32_t cut = build->log_length / 4u;
    while (cut < build->log_length && build->log[cut - 1u] != '\n') {
      ++cut;
    }
    memmove(build->log, build->log + cut, build->log_length - cut);
    build->log_length -= cut;
  }
  MemCopy(build->log + build->log_length, text, (uint32_t)length);
  build->log_length += (uint32_t)length;
}

VkrEditorBuild *vkr_editor_build_create(VkrAllocator *allocator) {
  VkrEditorBuild *build =
      vkr_allocator_alloc(allocator, sizeof(*build), BUILD_TAG);
  if (build) {
    MemZero(build, sizeof(*build));
    build->allocator = allocator;
  }
  return build;
}

static void build_game_join(VkrEditorBuild *build, bool8_t stop) {
  if (!build->game) {
    return;
  }
  if (stop) {
    vkr_atomic_bool_store(&build->game_stop, true_v, VKR_MEMORY_ORDER_RELEASE);
  }
  (void)vkr_thread_join(build->game);
  (void)vkr_thread_destroy(build->allocator, &build->game);
  build->game = NULL;
}

void vkr_editor_build_destroy(VkrEditorBuild *build) {
  if (!build) {
    return;
  }
  build_game_join(build, true_v);
  vkr_allocator_free(build->allocator, build, sizeof(*build), BUILD_TAG);
}

// =============================================================================
// Game settings draft
// =============================================================================

static void build_fields_load(VkrEditorBuild *build) {
  snprintf(build->width_text, sizeof(build->width_text), "%u",
           build->draft.window_width);
  snprintf(build->height_text, sizeof(build->height_text), "%u",
           build->draft.window_height);
  const VkrEditorGameProfile *profile = &build->draft.profiles[build->profile];
  build->include_text[0] = '\0';
  for (uint32_t i = 0; i < profile->include_count; ++i) {
    const uint32_t used = (uint32_t)strlen(build->include_text);
    snprintf(build->include_text + used, sizeof(build->include_text) - used,
             "%s%s", i ? ", " : "", profile->include[i]);
  }
  build->fields_profile = build->profile;
  build->fields_valid = true_v;
}

/* Comma-separated project paths into the selected profile's includes. */
static void build_includes_store(VkrEditorBuild *build) {
  VkrEditorGameProfile *profile = &build->draft.profiles[build->profile];
  profile->include_count = 0u;
  const char *start = build->include_text;
  for (const char *c = build->include_text;; ++c) {
    if (*c != ',' && *c) {
      continue;
    }
    while (start < c && *start == ' ') {
      ++start;
    }
    const char *end = c;
    while (end > start && end[-1] == ' ') {
      --end;
    }
    if (end > start && profile->include_count < VKR_EDITOR_GAME_INCLUDE_MAX &&
        (uint64_t)(end - start) < sizeof(profile->include[0])) {
      snprintf(profile->include[profile->include_count++],
               sizeof(profile->include[0]), "%.*s", (int)(end - start), start);
    }
    if (!*c) {
      break;
    }
    start = c + 1;
  }
}

static void build_load(VkrEditorBuild *build, const VkrEditorProject *project,
                       VkrAllocator *scratch) {
  VkrEditorProjectError error = {0};
  bool8_t exists = false_v;
  if (!vkr_editor_game_load(project, scratch, &build->draft, &exists, &error)) {
    snprintf(build->message, sizeof(build->message),
             "game.json is invalid (%s); the defaults are shown.",
             error.message);
  } else {
    build->message[0] = '\0';
  }
  build->saved = build->draft;
  build->profile = 0u;
  build_fields_load(build);
  snprintf(build->project_id, sizeof(build->project_id), "%s", project->id);
}

static bool8_t build_dirty(const VkrEditorBuild *build) {
  return MemCompare(&build->draft, &build->saved, sizeof(build->draft)) != 0;
}

static bool8_t build_save(VkrEditorBuild *build,
                          const VkrEditorProject *project, bool8_t read_only,
                          VkrAllocator *scratch) {
  if (!build_dirty(build)) {
    return true_v;
  }
  if (read_only) {
    snprintf(build->message, sizeof(build->message),
             "Read-only workspace: build settings are not saved.");
    return false_v;
  }
  VkrEditorProjectError error = {0};
  if (!vkr_editor_game_save(project, &build->draft, scratch, &error)) {
    snprintf(build->message, sizeof(build->message), "%s", error.message);
    return false_v;
  }
  build->saved = build->draft;
  return true_v;
}

// =============================================================================
// Job: events, report and Build and Run
// =============================================================================

static bool8_t build_event_text(VkrJsonReader line, const char *field,
                                char *out, uint32_t capacity) {
  out[0] = '\0';
  String8 value = {0};
  if (!vkr_json_find_root_field(&line, field) ||
      !vkr_json_parse_string(&line, &value)) {
    return false_v;
  }
  /* Event text escapes only quotes, backslashes and controls. */
  uint32_t written = 0u;
  for (uint64_t i = 0; i < value.length && written + 1u < capacity; ++i) {
    char c = (char)value.str[i];
    if (c == '\\' && i + 1u < value.length) {
      const char escape = (char)value.str[++i];
      c = escape == 'n' ? '\n' : escape == 't' ? '\t' : escape;
    }
    out[written++] = c;
  }
  out[written] = '\0';
  return true_v;
}

static float64_t build_event_number(VkrJsonReader line, const char *field) {
  float64_t value = -1.0;
  if (!vkr_json_find_root_field(&line, field) ||
      !vkr_json_parse_double(&line, &value)) {
    return -1.0;
  }
  return value;
}

static uint32_t build_stage_index(const char *name) {
  for (uint32_t i = 0; i < BUILD_STAGE_COUNT; ++i) {
    if (!strcmp(build_stage_names[i], name)) {
      return i;
    }
  }
  return UINT32_MAX;
}

/* One event line of the package job. */
static void build_apply_event(VkrEditorBuild *build, String8 bytes) {
  const VkrJsonReader line = vkr_json_reader_from_string(bytes);
  char event[16];
  char text[512];
  if (!build_event_text(line, "ev", event, sizeof(event))) {
    return;
  }
  if (!strcmp(event, "start")) {
    char source[64];
    (void)build_event_text(line, "source", source, sizeof(source));
    const uint32_t stage = build_stage_index(source);
    if (stage < BUILD_STAGE_COUNT) {
      build->stage = stage;
      build->stages[stage].status = BUILD_STAGE_RUNNING;
      build->fraction = -1.0;
      build->detail[0] = '\0';
    }
  } else if (!strcmp(event, "progress")) {
    build->fraction = build_event_number(line, "fraction");
    (void)build_event_text(line, "detail", build->detail,
                           sizeof(build->detail));
  } else if (!strcmp(event, "done")) {
    char status[16];
    (void)build_event_text(line, "status", status, sizeof(status));
    if (build->stage < BUILD_STAGE_COUNT) {
      BuildStage *stage = &build->stages[build->stage];
      stage->status =
          strcmp(status, "ok") ? BUILD_STAGE_FAILED : BUILD_STAGE_DONE;
      stage->seconds = build_event_number(line, "wall_ms") / 1000.0;
      build_log_line(build, "%s  %s  %.2f s",
                     strcmp(status, "ok") ? status : "done",
                     build_stage_names[build->stage], stage->seconds);
    }
  } else if (!strcmp(event, "diag")) {
    char code[32];
    char severity[16];
    char source[256];
    (void)build_event_text(line, "code", code, sizeof(code));
    (void)build_event_text(line, "severity", severity, sizeof(severity));
    (void)build_event_text(line, "source", source, sizeof(source));
    (void)build_event_text(line, "message", text, sizeof(text));
    build_log_line(build, "%s %s %s: %s", severity, code, source, text);
    if (!strcmp(severity, "error")) {
      log_error("Build %s %s: %s", code, source, text);
    } else {
      log_warn("Build %s %s: %s", code, source, text);
    }
  } else if (!strcmp(event, "log")) {
    char level[16];
    (void)build_event_text(line, "level", level, sizeof(level));
    (void)build_event_text(line, "text", text, sizeof(text));
    const uint64_t length = strlen(text);
    /* The report path is the job's last log line. */
    if (length > 5u && !strcmp(text + length - 5u, ".json") &&
        strstr(text, "/logs/builds/")) {
      snprintf(build->report_path, sizeof(build->report_path), "%s", text);
    } else {
      build_log_line(build, "%s%s", strcmp(level, "warn") ? "" : "warn  ",
                     text);
      if (!strcmp(level, "warn")) {
        log_warn("Build: %s", text);
      }
    }
  }
}

/* Applies the complete event lines appended since the last read. */
static void build_read_events(VkrEditorBuild *build) {
  FILE *file = file_fopen(build->events_path, "rb");
  if (!file) {
    return;
  }
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (FSEEK64(file, (int64_t)build->events_offset, SEEK_SET) == 0) {
    data = malloc(BUILD_READ_LIMIT + 1u);
    length = data ? fread(data, 1u, BUILD_READ_LIMIT, file) : 0u;
  }
  fclose(file);
  uint64_t start = 0u;
  for (uint64_t i = 0; i < length; ++i) {
    if (data[i] != '\n') {
      continue;
    }
    if (i > start && data[start] == '{') {
      build_apply_event(build,
                        (String8){.str = data + start, .length = i - start});
    }
    start = i + 1u;
  }
  build->events_offset += start;
  free(data);
}

/* The report names the package; its archives give the package size.
   Returns the report's bytes, borrowed from `scratch`, or an empty view. */
static String8 build_read_report(VkrEditorBuild *build, VkrAllocator *scratch) {
  String8 bytes = {0};
  if (!build->report_path[0]) {
    return bytes;
  }
  uint64_t fingerprint = 0u;
  VkrEditorProjectError error = {0};
  if (!vkr_editor_project_json_read_file(build->report_path, scratch, &bytes,
                                         &fingerprint, &error)) {
    return (String8){0};
  }
  char status[32];
  char output[BUILD_PATH];
  char executable[256];
  const bool8_t complete =
      vkr_editor_project_json_string(bytes, "status", status, sizeof(status),
                                     &error) &&
      !strcmp(status, "complete") &&
      vkr_editor_project_json_string(bytes, "output", output, sizeof(output),
                                     &error) &&
      vkr_editor_project_json_string(bytes, "executable", executable,
                                     sizeof(executable), &error);
  VkrJsonReader reader = vkr_json_reader_from_string(bytes);
  build->warning_count = 0u;
  if (vkr_json_find_root_field(&reader, "warnings") &&
      reader.pos < reader.length && reader.data[reader.pos] == '[') {
    ++reader.pos;
    String8 warning = {0};
    while (vkr_json_next_array_element(&reader) &&
           vkr_json_parse_string(&reader, &warning)) {
      ++build->warning_count;
    }
  }
  if (complete) {
    snprintf(build->output, sizeof(build->output), "%s", output);
    snprintf(build->executable, sizeof(build->executable), "%s/%s", output,
             executable);
    build->package_bytes = 0u;
    reader = vkr_json_reader_from_string(bytes);
    VkrJsonReader package = {0};
    if (vkr_json_find_root_field(&reader, "package") &&
        vkr_json_enter_object(&reader, &package) &&
        vkr_json_find_root_field(&package, "archives") &&
        package.pos < package.length && package.data[package.pos] == '[') {
      ++package.pos;
      while (vkr_json_next_array_element(&package)) {
        VkrJsonReader archive = {0};
        float64_t size = 0.0;
        if (!vkr_json_enter_object(&package, &archive)) {
          break;
        }
        if (vkr_json_find_root_field(&archive, "bytes") &&
            vkr_json_parse_double(&archive, &size)) {
          build->package_bytes += (uint64_t)size;
        }
      }
    }
  }
  return bytes;
}

static bool8_t build_game_cancelled(void *context) {
  VkrEditorBuild *build = context;
  return vkr_atomic_bool_load(&build->game_stop, VKR_MEMORY_ORDER_ACQUIRE);
}

static void *build_game_worker(void *argument) {
  VkrEditorBuild *build = argument;
  const VkrPlatformProcessConfig config = {
      .executable = build->game_path,
      .working_directory = build->game_directory,
      .stdout_path = build->game_log,
      .stderr_path = build->game_log,
      .append_output = true_v,
      .termination_grace_ms = 2000u,
      .terminate_process_tree = true_v,
      .is_cancelled = build_game_cancelled,
      .cancel_context = build,
  };
  bool8_t timed_out = false_v;
  build->game_exit = -1;
  (void)vkr_platform_process_run(&config, &build->game_exit, &timed_out);
  vkr_atomic_bool_store(&build->game_done, true_v, VKR_MEMORY_ORDER_RELEASE);
  return NULL;
}

/* Forwards the running game's new output lines to the Console. */
static void build_forward_game(VkrEditorBuild *build) {
  FILE *file = file_fopen(build->game_log, "rb");
  if (!file) {
    return;
  }
  uint8_t chunk[8192];
  size_t length = 0u;
  if (FSEEK64(file, (int64_t)build->game_log_offset, SEEK_SET) == 0) {
    length = fread(chunk, 1u, sizeof(chunk) - 1u, file);
  }
  fclose(file);
  uint64_t start = 0u;
  for (uint64_t i = 0; i < length; ++i) {
    if (chunk[i] != '\n') {
      continue;
    }
    if (i > start) {
      log_info("[game] %.*s", (int)(i - start), (const char *)chunk + start);
    }
    start = i + 1u;
  }
  /* A line longer than the chunk is forwarded in pieces. */
  if (!start && length == sizeof(chunk) - 1u) {
    log_info("[game] %.*s", (int)length, (const char *)chunk);
    start = length;
  }
  build->game_log_offset += start;
}

static bool8_t build_run_game(VkrEditorBuild *build, const char *workspace,
                              char *message, uint32_t capacity) {
  if (!build->executable[0]) {
    snprintf(message, capacity, "Build the game first.");
    return false_v;
  }
  if (build->game) {
    snprintf(message, capacity, "The game is already running.");
    return false_v;
  }
  snprintf(build->game_path, sizeof(build->game_path), "%s", build->executable);
  snprintf(build->game_directory, sizeof(build->game_directory), "%s",
           build->output);
  snprintf(build->game_log, sizeof(build->game_log), "%s/logs/builds/game.log",
           workspace);
  FilePath log_file = {.path = build_text(build->game_log),
                       .type = FILE_PATH_TYPE_ABSOLUTE};
  (void)file_remove(&log_file);
  build->game_log_offset = 0u;
  vkr_atomic_bool_store(&build->game_done, false_v, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_bool_store(&build->game_stop, false_v, VKR_MEMORY_ORDER_RELAXED);
  if (!vkr_thread_create(build->allocator, &build->game, build_game_worker,
                         build)) {
    build->game = NULL;
    snprintf(message, capacity, "The game could not start.");
    return false_v;
  }
  log_info("Build: running %s", build->game_path);
  snprintf(message, capacity, "Running the game");
  return true_v;
}

static void build_finish(VkrEditorBuild *build, VkrEditorUi *editor,
                         const VkrSampleUiFrame *frame,
                         VkrEditorProjectJobStatus status) {
  build_read_events(build);
  const String8 report = build_read_report(build, frame->ui->frame_allocator);
  build->phase = BUILD_FINISHED;
  build->job_id = 0u;
  build->succeeded = status == VKR_EDITOR_PROJECT_JOB_SUCCEEDED;
  if (build->succeeded) {
    const float64_t mib = (float64_t)build->package_bytes / (1024.0 * 1024.0);
    if (build->warning_count) {
      snprintf(build->result, sizeof(build->result),
               "Built %s, %.1f MiB, %u warning%s", build->output, mib,
               build->warning_count, build->warning_count == 1u ? "" : "s");
    } else {
      snprintf(build->result, sizeof(build->result), "Built %s, %.1f MiB",
               build->output, mib);
    }
    log_info("Build: %s", build->result);
    /* A shipping build that finalized project assets returns their new
       inventory; the editor publishes it, as after its own finalize. */
    String8 assets = {0};
    VkrEditorProjectError error = {0};
    if (report.length && vkr_editor_project_json_member(
                             report, "project_assets", &assets, &error)) {
      (void)vkr_editor_projects_adopt_inventory(editor->projects, editor, frame,
                                                assets, build->assets_digest);
    }
    vkr_editor_toast(editor, VKR_UI_ICON_CHECK_CIRCLE, vkr_ui_theme()->success,
                     "Build complete");
    if (build->run_after) {
      char message[128];
      (void)build_run_game(build,
                           vkr_editor_projects_workspace_root(editor->projects),
                           message, sizeof(message));
    }
  } else {
    snprintf(build->result, sizeof(build->result), "%s",
             status == VKR_EDITOR_PROJECT_JOB_CANCELLED
                 ? "Build cancelled; the previous package is unchanged."
                 : "Build failed; the previous package is unchanged. See "
                   "the log.");
    log_error("Build: %s", build->result);
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, vkr_ui_theme()->warning,
                     status == VKR_EDITOR_PROJECT_JOB_CANCELLED
                         ? "Build cancelled"
                         : "Build failed");
  }
}

static bool8_t build_begin_job(VkrEditorBuild *build, VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  const VkrEditorProject *project =
      vkr_editor_projects_project(editor->projects);
  const char *workspace = vkr_editor_projects_workspace_root(editor->projects);
  const char *slash = project ? strrchr(project->manifest_path, '/') : NULL;
  if (!slash) {
    snprintf(build->result, sizeof(build->result), "No project is open.");
    return false_v;
  }
  snprintf(build->project_directory, sizeof(build->project_directory), "%.*s",
           (int)(slash - project->manifest_path), project->manifest_path);
  char directory[BUILD_PATH];
  snprintf(directory, sizeof(directory), "%s/logs/builds", workspace);
  String8 directory_text = build_text(directory);
  snprintf(build->events_path, sizeof(build->events_path),
           "%s/last.events.jsonl", directory);
  if (!file_ensure_directory(frame->ui->frame_allocator, &directory_text)) {
    snprintf(build->result, sizeof(build->result),
             "Cannot create the build log folder.");
    return false_v;
  }
  build->job_id =
      vkr_editor_bakery_package_start(editor->bakery, build->project_directory,
                                      build->profile_name, build->events_path);
  if (!build->job_id) {
    snprintf(build->result, sizeof(build->result),
             "Bakery's queue is full; try again when a job finishes.");
    return false_v;
  }
  vkr_sha256(project->assets.str, project->assets.length, build->assets_digest);
  build->events_offset = 0u;
  build->phase = BUILD_RUNNING;
  log_info("Build: packaging %s with profile %s", project->name,
           build->profile_name);
  vkr_editor_dock_show(frame->dock, VKR_UI_DOCK_PANEL_BUILD);
  return true_v;
}

void vkr_editor_build_update(VkrEditorBuild *build, VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame) {
  if (!build) {
    return;
  }
  const VkrEditorProject *project =
      vkr_editor_projects_project(editor->projects);
  if (project && strcmp(build->project_id, project->id) != 0 &&
      build->phase != BUILD_RUNNING) {
    build_load(build, project, frame->ui->frame_allocator);
  }
  if (build->game) {
    build_forward_game(build);
    if (vkr_atomic_bool_load(&build->game_done, VKR_MEMORY_ORDER_ACQUIRE)) {
      build_game_join(build, false_v);
      build_forward_game(build);
      log_info("Build: the game exited with code %d", build->game_exit);
    }
  }
  if (build->phase == BUILD_PREFLIGHT) {
    const VkrEditorBuildPreflight preflight =
        vkr_editor_projects_build_preflight(editor->projects, editor, frame);
    if (preflight == VKR_EDITOR_BUILD_PREFLIGHT_READY) {
      if (!build_begin_job(build, editor, frame)) {
        build->phase = BUILD_FINISHED;
        build->succeeded = false_v;
      }
    } else if (preflight != VKR_EDITOR_BUILD_PREFLIGHT_WAITING) {
      build->phase = BUILD_IDLE;
      snprintf(build->result, sizeof(build->result), "Build cancelled.");
    }
    return;
  }
  if (build->phase != BUILD_RUNNING) {
    return;
  }
  const float64_t now = vkr_platform_get_absolute_time();
  if (now >= build->next_read) {
    build->next_read = now + 0.2;
    build_read_events(build);
  }
  const VkrEditorProjectJobStatus status =
      vkr_editor_bakery_project_status(editor->bakery, build->job_id, NULL);
  if (status >= VKR_EDITOR_PROJECT_JOB_SUCCEEDED) {
    build_finish(build, editor, frame, status);
  }
}

bool8_t vkr_editor_build_start(VkrEditorBuild *build, VkrEditorUi *editor,
                               bool8_t run, char *message, uint32_t capacity) {
  const VkrEditorProject *project =
      build ? vkr_editor_projects_project(editor->projects) : NULL;
  if (!project) {
    snprintf(message, capacity, "Open a project to build it.");
    return false_v;
  }
  if (build->phase == BUILD_PREFLIGHT || build->phase == BUILD_RUNNING) {
    snprintf(message, capacity, "A build is already running.");
    return false_v;
  }
  if (!build->fields_valid) {
    build_fields_load(build);
  }
  build_includes_store(build);
  VkrEditorProjectError error = {0};
  if (!vkr_editor_game_validate(&build->draft, project, &error)) {
    snprintf(message, capacity, "%s", error.message);
    return false_v;
  }
  const VkrEditorGameProfile *profile = &build->draft.profiles[build->profile];
  if (!profile->output[0]) {
    snprintf(message, capacity,
             "Choose an output folder in Build Settings first.");
    vkr_editor_window_set_visible(editor, VKR_EDITOR_WINDOW_BUILD, true_v);
    return false_v;
  }
  /* The package reads game.json, so the draft is saved first. */
  Arena *arena = arena_create(MB(2), KB(256));
  VkrAllocator scratch = {.ctx = arena};
  const bool8_t saved =
      arena && vkr_allocator_arena(&scratch) &&
      build_save(build, project,
                 vkr_editor_projects_read_only(editor->projects), &scratch);
  if (arena) {
    vkr_allocator_release_global_accounting(&scratch);
    arena_destroy(arena);
  }
  if (!saved) {
    snprintf(message, capacity, "%s",
             build->message[0] ? build->message : "Cannot save game.json.");
    return false_v;
  }
  snprintf(build->profile_name, sizeof(build->profile_name), "%s",
           profile->name);
  build->run_after = run;
  build->phase = BUILD_PREFLIGHT;
  build->succeeded = false_v;
  build->report_path[0] = '\0';
  build->result[0] = '\0';
  build->log_length = 0u;
  build->stage = UINT32_MAX;
  build->fraction = -1.0;
  build->detail[0] = '\0';
  MemZero(build->stages, sizeof(build->stages));
  snprintf(message, capacity, "Building %s (%s)", build->draft.name,
           profile->name);
  return true_v;
}

void vkr_editor_build_cancel(VkrEditorBuild *build, VkrEditorUi *editor) {
  if (!build) {
    return;
  }
  if (build->phase == BUILD_PREFLIGHT) {
    build->phase = BUILD_IDLE;
  } else if (build->phase == BUILD_RUNNING) {
    vkr_editor_bakery_project_cancel(editor->bakery, build->job_id);
  }
}

bool8_t vkr_editor_build_open_last(VkrEditorBuild *build, char *message,
                                   uint32_t capacity) {
  if (!build || !build->output[0]) {
    snprintf(message, capacity, "No build yet in this session.");
    return false_v;
  }
#if defined(PLATFORM_WINDOWS)
  const char *executable = "explorer.exe";
  const char *arguments[] = {build->output};
#else
  const char *executable = "/usr/bin/open";
  const char *arguments[] = {build->output};
#endif
  const VkrPlatformProcessConfig config = {.executable = executable,
                                           .arguments = arguments,
                                           .argument_count = 1u,
                                           .timeout_ms = 5000u,
                                           .termination_grace_ms = 250u};
  int32_t code = -1;
  bool8_t timed_out = false_v;
  if (!vkr_platform_process_run(&config, &code, &timed_out)) {
    snprintf(message, capacity, "Cannot open %s", build->output);
    return false_v;
  }
  snprintf(message, capacity, "Opened %s", build->output);
  return true_v;
}

bool8_t vkr_editor_build_available(const VkrEditorBuild *build,
                                   const VkrEditorUi *editor) {
  return build && vkr_editor_projects_project(editor->projects) &&
         !vkr_editor_build_busy(build);
}

bool8_t vkr_editor_build_busy(const VkrEditorBuild *build) {
  return build &&
         (build->phase == BUILD_PREFLIGHT || build->phase == BUILD_RUNNING);
}

bool8_t vkr_editor_build_has_package(const VkrEditorBuild *build) {
  return build && build->output[0];
}

bool8_t vkr_editor_build_select_profile(VkrEditorBuild *build,
                                        const char *name) {
  for (uint32_t i = 0; build && i < build->draft.profile_count; ++i) {
    if (!strcmp(build->draft.profiles[i].name, name)) {
      if (build->fields_valid) {
        build_includes_store(build);
      }
      build->profile = i;
      build_fields_load(build);
      return true_v;
    }
  }
  return false_v;
}

const char *vkr_editor_build_result(const VkrEditorBuild *build,
                                    bool8_t *out_succeeded) {
  *out_succeeded = build && build->phase == BUILD_FINISHED && build->succeeded;
  return build ? build->result : "";
}

// =============================================================================
// Build Settings window
// =============================================================================

static VkrUiWidgetConfig build_widget(float32_t x, float32_t y, float32_t width,
                                      float32_t height) {
  VkrUiWidgetConfig c = vkr_ui_widget_config_default();
  c.placement = (VkrUiPlacement){.column_span = 1,
                                 .row_span = 1,
                                 .justify = VKR_UI_ALIGN_START,
                                 .align = VKR_UI_ALIGN_START,
                                 .margin_pt = {y, 0, 0, x}};
  c.style.min_size_pt = (Vec2){Max(1.0f, width), height};
  c.style.max_size_pt = c.style.min_size_pt;
  c.style.font_size_pt = vkr_ui_theme()->font_body;
  c.style.text_color = vkr_ui_theme()->text;
  c.style.padding_pt = (VkrUiEdges){2, 4, 2, 4};
  return c;
}

static void build_label(VkrUiSystem *ui, const char *id, String8 text,
                        float32_t x, float32_t y, float32_t width,
                        bool8_t secondary) {
  VkrUiWidgetConfig c = build_widget(x, y, width, 24);
  c.style.text_color =
      secondary ? vkr_ui_theme()->text_secondary : vkr_ui_theme()->text;
  vkr_ui_label(ui, build_text(id), text, &c);
}

static float32_t build_section(VkrUiSystem *ui, VkrFontHandle heading,
                               const char *id, const char *title, float32_t y,
                               float32_t width) {
  VkrUiWidgetConfig c = vkr_editor_section_label_config(heading);
  c.placement = build_widget(12, y, width - 24, 22).placement;
  c.style.min_size_pt = (Vec2){width - 24, 22};
  c.style.max_size_pt = c.style.min_size_pt;
  vkr_ui_label(ui, build_text(id), build_text(title), &c);
  return y + 28;
}

/* A labelled text field; true when it changed. */
static bool8_t build_field(VkrUiSystem *ui, const char *id, const char *label,
                           char *text, uint32_t capacity, float32_t y,
                           float32_t width, float32_t label_width) {
  build_label(ui, id, build_text(label), 12, y, label_width, true_v);
  VkrUiWidgetConfig c =
      build_widget(12 + label_width, y, width - 24 - label_width, 24);
  vkr_editor_field_style(&c);
  VkrUiTextEditBuffer buffer = {(uint8_t *)text, (uint32_t)strlen(text),
                                capacity};
  char field_id[64];
  snprintf(field_id, sizeof(field_id), "%s.field", id);
  return vkr_ui_text_field(ui, build_text(field_id), &buffer, &c);
}

static const char *build_scene_name(const VkrEditorProject *project,
                                    const char *id) {
  for (uint32_t i = 0; i < project->scene_count; ++i) {
    if (!strcmp(project->scenes[i].id, id)) {
      return project->scenes[i].name;
    }
  }
  return id;
}

static uint32_t build_scene_included(const VkrEditorGame *game,
                                     const char *id) {
  for (uint32_t i = 0; i < game->scene_count; ++i) {
    if (!strcmp(game->scenes[i], id)) {
      return i;
    }
  }
  return UINT32_MAX;
}

static void build_settings_profiles(VkrEditorBuild *build, VkrUiSystem *ui,
                                    VkrFontHandle heading, float32_t *y,
                                    float32_t width) {
  VkrEditorGame *game = &build->draft;
  *y = build_section(ui, heading, "profiles.title", "Profiles", *y, width);
  float32_t x = 12;
  for (uint32_t i = 0; i < game->profile_count; ++i) {
    const bool8_t selected = build->profile == i;
    const float32_t w =
        Min(160.0f, 20.0f + 7.5f * strlen(game->profiles[i].name));
    if (x + w > width - 120) {
      break;
    }
    VkrUiWidgetConfig c = build_widget(x, *y, w, 26);
    vkr_editor_toggle_style(&c, selected);
    c.text.font = selected ? heading : VKR_FONT_HANDLE_INVALID;
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("profile"),
                      build_text(game->profiles[i].name), &c) &&
        !selected) {
      build_includes_store(build);
      build->profile = i;
      build_fields_load(build);
    }
    (void)vkr_ui_pop_id(ui);
    x += w + 4;
  }
  const char *actions[] = {"Add", "Duplicate", "Delete"};
  const VkrUiIcon icons[] = {VKR_UI_ICON_ADD, VKR_UI_ICON_DUPLICATE,
                             VKR_UI_ICON_TRASH};
  for (uint32_t i = 0; i < ArrayCount(actions); ++i) {
    VkrUiWidgetConfig c =
        vkr_editor_icon_button_config(0, 0, icons[i], build_text(actions[i]));
    c.placement = build_widget(width - 102 + i * 30, *y, 26, 26).placement;
    c.disabled = i < 2 ? game->profile_count == VKR_EDITOR_GAME_PROFILE_MAX
                       : game->profile_count == 1u;
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("profile.action"), (String8){0}, &c)) {
      build_includes_store(build);
      if (i == 0u) {
        vkr_editor_game_profile_default("",
                                        &game->profiles[game->profile_count]);
        snprintf(game->profiles[game->profile_count].name,
                 sizeof(game->profiles[0].name), "Profile %u",
                 game->profile_count + 1u);
        build->profile = game->profile_count++;
      } else if (i == 1u) {
        game->profiles[game->profile_count] = game->profiles[build->profile];
        char *name = game->profiles[game->profile_count].name;
        const uint32_t length = (uint32_t)strlen(name);
        snprintf(name + Min(length, 56u), sizeof(game->profiles[0].name) - 56u,
                 " copy");
        build->profile = game->profile_count++;
      } else {
        MemCopy(&game->profiles[build->profile],
                &game->profiles[build->profile + 1u],
                (game->profile_count - build->profile - 1u) *
                    sizeof(game->profiles[0]));
        --game->profile_count;
        MemZero(&game->profiles[game->profile_count],
                sizeof(game->profiles[0]));
        build->profile = Min(build->profile, game->profile_count - 1u);
      }
      build_fields_load(build);
    }
    (void)vkr_ui_pop_id(ui);
  }
  *y += 34;
  VkrEditorGameProfile *profile = &game->profiles[build->profile];
  (void)build_field(ui, "profile.name", "Profile name", profile->name,
                    sizeof(profile->name), *y, width, 120);
  *y += 30;
}

static void build_settings_game(VkrEditorBuild *build, VkrUiSystem *ui,
                                VkrFontHandle heading, float32_t *y,
                                float32_t width) {
  VkrEditorGame *game = &build->draft;
  *y = build_section(ui, heading, "game.title", "Game", *y, width);
  (void)build_field(ui, "game.name", "Name", game->name, sizeof(game->name), *y,
                    width, 120);
  *y += 30;
  (void)build_field(ui, "game.version", "Version", game->version,
                    sizeof(game->version), *y, width, 120);
  *y += 30;
  (void)build_field(ui, "game.company", "Company", game->company,
                    sizeof(game->company), *y, width, 120);
  *y += 30;
  (void)build_field(ui, "game.executable", "Executable", game->executable,
                    sizeof(game->executable), *y, width, 120);
  *y += 30;
  build_label(ui, "window.label", string8_lit("Window"), 12, *y, 120, true_v);
  const float32_t half = (width - 24 - 120 - 24) * 0.5f;
  VkrUiWidgetConfig c = build_widget(132, *y, half, 24);
  vkr_editor_field_style(&c);
  VkrUiTextEditBuffer w = {(uint8_t *)build->width_text,
                           (uint32_t)strlen(build->width_text),
                           sizeof(build->width_text)};
  if (vkr_ui_text_field(ui, string8_lit("window.width"), &w, &c)) {
    game->window_width = (uint32_t)strtoul(build->width_text, NULL, 10);
  }
  build_label(ui, "window.by", string8_lit("\xc3\x97"), 136 + half, *y, 16,
              true_v);
  c = build_widget(156 + half, *y, half, 24);
  vkr_editor_field_style(&c);
  VkrUiTextEditBuffer h = {(uint8_t *)build->height_text,
                           (uint32_t)strlen(build->height_text),
                           sizeof(build->height_text)};
  if (vkr_ui_text_field(ui, string8_lit("window.height"), &h, &c)) {
    game->window_height = (uint32_t)strtoul(build->height_text, NULL, 10);
  }
  *y += 30;
  build_label(ui, "mode.label", string8_lit("Display"), 12, *y, 120, true_v);
  const char *modes[] = {"windowed", "fullscreen", "borderless"};
  const char *mode_labels[] = {"Windowed", "Fullscreen", "Borderless"};
  const char *mode_tips[] = {
      "A window of the size above",
      "The platform's fullscreen: its own Space on macOS",
      "A borderless window covering the display"};
  for (uint32_t i = 0; i < ArrayCount(modes); ++i) {
    const bool8_t selected = !strcmp(game->window_mode, modes[i]);
    c = build_widget(132 + i * 124, *y, 120, 24);
    vkr_editor_toggle_style(&c, selected);
    c.tooltip = build_text(mode_tips[i]);
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("mode"), build_text(mode_labels[i]),
                      &c)) {
      snprintf(game->window_mode, sizeof(game->window_mode), "%s", modes[i]);
    }
    (void)vkr_ui_pop_id(ui);
  }
  *y += 30;
}

static void build_settings_scenes(VkrEditorBuild *build, VkrUiSystem *ui,
                                  VkrFontHandle heading,
                                  const VkrEditorProject *project, float32_t *y,
                                  float32_t width) {
  VkrEditorGame *game = &build->draft;
  const VkrUiTheme *theme = vkr_ui_theme();
  *y = build_section(ui, heading, "scenes.title", "Scenes", *y, width);
  bool8_t world = true_v;
  VkrUiWidgetConfig c = build_widget(12, *y, width - 24, 24);
  c.disabled = true_v;
  c.tooltip = string8_lit("The World is always included");
  (void)vkr_ui_checkbox(ui, string8_lit("scene.world"),
                        string8_lit("World (always included)"), &world, &c);
  *y += 28;
  /* Included scenes in their order, then the others. */
  for (uint32_t pass = 0; pass < 2; ++pass) {
    const uint32_t count = pass ? project->scene_count : game->scene_count;
    for (uint32_t i = 0; i < count; ++i) {
      const char *id = pass ? project->scenes[i].id : game->scenes[i];
      const uint32_t index = build_scene_included(game, id);
      if (pass && index != UINT32_MAX) {
        continue;
      }
      (void)vkr_ui_push_id_u64(ui, pass * 1000u + i);
      bool8_t included = index != UINT32_MAX;
      c = build_widget(12, *y, width - 190, 24);
      if (vkr_ui_checkbox(ui, string8_lit("scene.include"),
                          build_text(build_scene_name(project, id)), &included,
                          &c)) {
        if (included && game->scene_count < VKR_EDITOR_PROJECT_MAX_SCENES) {
          snprintf(game->scenes[game->scene_count++], sizeof(game->scenes[0]),
                   "%s", id);
          if (!game->startup_scene[0]) {
            snprintf(game->startup_scene, sizeof(game->startup_scene), "%s",
                     id);
          }
        } else if (!included && index != UINT32_MAX) {
          MemCopy(game->scenes[index], game->scenes[index + 1u],
                  (game->scene_count - index - 1u) * sizeof(game->scenes[0]));
          --game->scene_count;
          if (!strcmp(game->startup_scene, id)) {
            snprintf(game->startup_scene, sizeof(game->startup_scene), "%s",
                     game->scene_count ? game->scenes[0] : "");
          }
        }
      }
      if (!pass) {
        const bool8_t startup = !strcmp(game->startup_scene, id);
        c = build_widget(width - 174, *y, 90, 24);
        vkr_editor_toggle_style(&c, startup);
        c.style.font_size_pt = theme->font_caption;
        c.icon = startup ? VKR_UI_ICON_STAR_FILL : VKR_UI_ICON_STAR;
        c.icon_size_pt = 12.0f;
        c.tooltip = string8_lit("The scene the game starts in");
        if (vkr_ui_button(ui, string8_lit("scene.startup"),
                          string8_lit("Startup"), &c)) {
          snprintf(game->startup_scene, sizeof(game->startup_scene), "%s", id);
        }
        const VkrUiIcon arrows[] = {VKR_UI_ICON_ARROW_UP,
                                    VKR_UI_ICON_ARROW_DOWN};
        for (uint32_t a = 0; a < 2; ++a) {
          c = vkr_editor_icon_button_config(
              0, 0, arrows[a],
              a ? string8_lit("Later in the order")
                : string8_lit("Earlier in the order"));
          c.placement = build_widget(width - 78 + a * 30, *y, 26, 24).placement;
          c.disabled = a ? i + 1u >= game->scene_count : i == 0u;
          (void)vkr_ui_push_id_u64(ui, a);
          if (vkr_ui_button(ui, string8_lit("scene.move"), (String8){0}, &c)) {
            const uint32_t other = a ? i + 1u : i - 1u;
            char swap[37];
            MemCopy(swap, game->scenes[i], sizeof(swap));
            MemCopy(game->scenes[i], game->scenes[other], sizeof(swap));
            MemCopy(game->scenes[other], swap, sizeof(swap));
          }
          (void)vkr_ui_pop_id(ui);
        }
      }
      (void)vkr_ui_pop_id(ui);
      *y += 28;
    }
  }
}

static void build_settings_target(VkrEditorBuild *build, VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame, float32_t *y,
                                  float32_t width, bool8_t *out_dialog) {
  VkrUiSystem *ui = frame->ui;
  VkrFontHandle heading = editor->heading_font;
  VkrEditorGameProfile *profile = &build->draft.profiles[build->profile];
  *y = build_section(ui, heading, "target.title", "Target", *y, width);
  build_label(ui, "platform.label", string8_lit("Platform"), 12, *y, 120,
              true_v);
  VkrUiWidgetConfig c = build_widget(132, *y, width - 144, 24);
  c.style.text_color = vkr_ui_theme()->text;
  c.tooltip = string8_lit("Each platform packages on its own host: Metal "
                          "needs the macOS toolchain");
  vkr_ui_label(ui, string8_lit("platform.value"),
               string8_lit("This computer (" BUILD_HOST_PLATFORM
                           "); other platforms build on their own host"),
               &c);
  *y += 30;
  build_label(ui, "config.label", string8_lit("Configuration"), 12, *y, 120,
              true_v);
  const char *configs[] = {"development", "shipping"};
  const char *labels[] = {"Development", "Shipping"};
  const char *tips[] = {"Info logging and the F6 debug overlay",
                        "Errors only and no developer UI"};
  for (uint32_t i = 0; i < 2; ++i) {
    const bool8_t selected = !strcmp(profile->config, configs[i]);
    c = build_widget(132 + i * 124, *y, 120, 24);
    vkr_editor_toggle_style(&c, selected);
    c.tooltip = build_text(tips[i]);
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("config"), build_text(labels[i]), &c)) {
      snprintf(profile->config, sizeof(profile->config), "%s", configs[i]);
    }
    (void)vkr_ui_pop_id(ui);
  }
  *y += 30;
  build_label(ui, "output.label", string8_lit("Output folder"), 12, *y, 120,
              true_v);
  c = build_widget(132, *y, width - 144 - 92, 24);
  vkr_editor_field_style(&c);
  VkrUiTextEditBuffer output = {(uint8_t *)profile->output,
                                (uint32_t)strlen(profile->output),
                                sizeof(profile->output)};
  (void)vkr_ui_text_field(ui, string8_lit("output.field"), &output, &c);
  c = build_widget(width - 100, *y, 88, 24);
  vkr_editor_action_style(&c, heading);
  c.icon = VKR_UI_ICON_FOLDER;
  c.icon_size_pt = 13.0f;
  if (vkr_ui_button(ui, string8_lit("output.choose"), string8_lit("Choose"),
                    &c)) {
    VkrFileDialogRequest request = {.kind = VKR_FILE_DIALOG_OPEN_FOLDER,
                                    .title = "Choose the build output folder"};
    VkrFileDialogResult result = {0};
    vkr_file_dialog_show(frame->window, &request, build->allocator, &result);
    if (result.status == VKR_FILE_DIALOG_SELECTED && result.path_count == 1u &&
        strlen(result.paths[0]) + 1u + strlen(build->draft.executable) <
            sizeof(profile->output)) {
      /* The package is its own folder inside the chosen one. */
      snprintf(profile->output, sizeof(profile->output), "%s/%s",
               result.paths[0], build->draft.executable);
    }
    vkr_file_dialog_result_destroy(build->allocator, &result);
    *out_dialog = true_v;
  }
  *y += 30;
  build_label(ui, "include.label", string8_lit("Extra includes"), 12, *y, 120,
              true_v);
  c = build_widget(132, *y, width - 144, 24);
  vkr_editor_field_style(&c);
  c.tooltip = string8_lit("Comma-separated files or folders below the "
                          "project directory that ship even when no scene "
                          "names them");
  VkrUiTextEditBuffer include = {(uint8_t *)build->include_text,
                                 (uint32_t)strlen(build->include_text),
                                 sizeof(build->include_text)};
  if (vkr_ui_text_field(ui, string8_lit("include.field"), &include, &c)) {
    build_includes_store(build);
  }
  *y += 30;
}

typedef struct BuildTextSink {
  char *bytes;
  uint32_t capacity;
  uint32_t length;
} BuildTextSink;

static bool8_t build_text_sink(void *context, const uint8_t *data,
                               uint64_t length) {
  BuildTextSink *sink = context;
  if (length > sink->capacity - sink->length) {
    return false_v;
  }
  MemCopy(sink->bytes + sink->length, data, length);
  sink->length += (uint32_t)length;
  return true_v;
}

static void build_settings_options(VkrEditorBuild *build, VkrUiSystem *ui,
                                   VkrFontHandle heading,
                                   const VkrSampleUiFrame *frame, float32_t *y,
                                   float32_t width) {
  VkrEditorGameProfile *profile = &build->draft.profiles[build->profile];
  *y = build_section(ui, heading, "options.title", "Options", *y, width);
  VkrUiWidgetConfig c = build_widget(12, *y, width - 24, 24);
  c.tooltip = string8_lit("Bake reflection probes and diffuse lighting of "
                          "every included scene before packaging");
  (void)vkr_ui_checkbox(ui, string8_lit("option.bake"),
                        string8_lit("Bake lighting"), &profile->bake_lighting,
                        &c);
  *y += 28;
  c = build_widget(12, *y, width - 24, 24);
  (void)vkr_ui_checkbox(ui, string8_lit("option.run"),
                        string8_lit("Run after build"),
                        &profile->run_after_build, &c);
  *y += 28;
  c = build_widget(12, *y, 250, 24);
  vkr_editor_action_style(&c, heading);
  c.icon = VKR_UI_ICON_GRAPHICS;
  c.icon_size_pt = 13.0f;
  c.tooltip = string8_lit("The game starts with these Graphics settings; "
                          "players can change them later");
  if (vkr_ui_button(ui, string8_lit("option.graphics"),
                    string8_lit("Use current Graphics settings"), &c) &&
      frame->graphics) {
    char text[VKR_EDITOR_GAME_GRAPHICS_CAPACITY];
    BuildTextSink sink = {.bytes = text, .capacity = sizeof(text) - 1u};
    VkrJsonWriter writer;
    vkr_json_writer_init(&writer, build_text_sink, &sink);
    if (vkr_graphics_settings_write_json(&writer, &frame->graphics->settings) &&
        vkr_json_writer_complete(&writer)) {
      text[sink.length] = '\0';
      snprintf(build->draft.graphics, sizeof(build->draft.graphics), "%s",
               text);
    }
  }
  c = build_widget(270, *y, width - 282, 24);
  c.style.text_color = vkr_ui_theme()->text_secondary;
  c.style.font_size_pt = vkr_ui_theme()->font_caption;
  vkr_ui_label(ui, string8_lit("option.graphics.state"),
               strcmp(build->draft.graphics, "{\"version\":1}")
                   ? string8_lit("The game starts with saved Graphics "
                                 "settings")
                   : string8_lit("The game starts with the default Graphics "
                                 "settings"),
               &c);
  *y += 30;
}

void vkr_editor_build_settings_build(VkrEditorBuild *build, VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     VkrUiRect bounds) {
  VkrUiSystem *ui = frame->ui;
  const VkrEditorProject *project =
      vkr_editor_projects_project(editor->projects);
  const float32_t width = bounds.width / ui->content_scale;
  const float32_t height = bounds.height / ui->content_scale;
  if (!build || width < 320 || height < 120) {
    return;
  }
  if (!project) {
    build_label(ui, "none", string8_lit("Open a project to set up its build."),
                12, 12, width - 24, true_v);
    return;
  }
  if (!build->fields_valid || build->fields_profile != build->profile) {
    build_fields_load(build);
  }
  const float32_t footer = 84.0f;
  const float32_t content_height = 590.0f + 28.0f * (project->scene_count + 1u);
  if (!ui->mouse_captured && ui->mouse_input_layer == ui->input_layer &&
      ui->mouse_x >= bounds.x && ui->mouse_x < bounds.x + bounds.width &&
      ui->mouse_y >= bounds.y &&
      ui->mouse_y < bounds.y + bounds.height - footer * ui->content_scale) {
    build->settings_scroll -= ui->mouse_wheel * 40;
  }
  build->settings_scroll = vkr_clamp_f32(
      build->settings_scroll, 0, Max(0.0f, content_height - (height - footer)));
  const VkrUiTrack rows[] = {
      {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
      {.value = footer, .unit = VKR_UI_TRACK_PX},
  };
  VkrUiPanelConfig layout = vkr_ui_panel_config_default();
  layout.rows = rows;
  layout.row_count = ArrayCount(rows);
  layout.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("build.settings"), &layout)) {
    return;
  }
  VkrUiPanelConfig scroll = vkr_ui_panel_config_default();
  const VkrUiTrack track = {.value = content_height, .unit = VKR_UI_TRACK_PX};
  scroll.placement.row = 0;
  scroll.rows = &track;
  scroll.row_count = 1;
  scroll.clip_children = true_v;
  bool8_t dialog = false_v;
  if (vkr_ui_scroll_area_begin(ui, string8_lit("build.settings.scroll"),
                               &scroll)) {
    (void)vkr_ui_scroll_area_offset(ui, &build->settings_scroll);
    float32_t y = 10;
    build_settings_profiles(build, ui, editor->heading_font, &y, width);
    build_settings_game(build, ui, editor->heading_font, &y, width);
    build_settings_scenes(build, ui, editor->heading_font, project, &y, width);
    build_settings_target(build, editor, frame, &y, width, &dialog);
    if (!dialog) {
      build_settings_options(build, ui, editor->heading_font, frame, &y, width);
    }
    vkr_ui_scroll_area_end(ui);
  }
  if (dialog) {
    /* A native dialog covered this frame; the rest waits for the next. */
    (void)vkr_ui_panel_end(ui);
    return;
  }

  /* Footer: validation, last package, Save, Build and Build and Run. */
  VkrUiPanelConfig bar = vkr_ui_panel_config_default();
  bar.placement.row = 1;
  bar.style.border_pt = (VkrUiEdges){1, 0, 0, 0};
  bar.style.border_color = vkr_ui_theme()->separator;
  if (vkr_ui_panel_begin(ui, string8_lit("build.footer"), &bar)) {
    const VkrUiTheme *theme = vkr_ui_theme();
    VkrEditorProjectError error = {0};
    build_includes_store(build);
    const bool8_t valid =
        vkr_editor_game_validate(&build->draft, project, &error);
    const VkrEditorGameProfile *profile =
        &build->draft.profiles[build->profile];
    const char *status = build->message[0]     ? build->message
                         : !valid              ? error.message
                         : !profile->output[0] ? "Choose an output folder."
                         : vkr_editor_build_busy(build) ? "Building..."
                         : build->result[0]             ? build->result
                         : build_dirty(build)
                             ? "Unsaved build settings; Build saves them."
                             : "Ready to build.";
    VkrUiWidgetConfig c = build_widget(12, 8, width - 24, 22);
    c.style.font_size_pt = theme->font_caption;
    c.style.text_color =
        !valid || build->message[0] ? theme->warning : theme->text_secondary;
    c.icon = !valid ? VKR_UI_ICON_WARNING_FILL : VKR_UI_ICON_INFO_FILL;
    c.icon_size_pt = 12.0f;
    vkr_ui_label(ui, string8_lit("status"), build_text(status), &c);
    c = build_widget(12, 42, 90, 28);
    vkr_editor_action_style(&c, editor->heading_font);
    c.disabled =
        !build_dirty(build) || vkr_editor_projects_read_only(editor->projects);
    c.icon = VKR_UI_ICON_SAVE;
    c.icon_size_pt = 13.0f;
    if (vkr_ui_button(ui, string8_lit("save"), string8_lit("Save"), &c)) {
      (void)build_save(build, project,
                       vkr_editor_projects_read_only(editor->projects),
                       frame->ui->frame_allocator);
    }
    char message[256] = {0};
    c = build_widget(width - 272, 42, 120, 28);
    vkr_editor_action_style(&c, editor->heading_font);
    c.disabled = !valid || vkr_editor_build_busy(build);
    c.icon = VKR_UI_ICON_EXPORT;
    c.icon_size_pt = 13.0f;
    if (vkr_ui_button(ui, string8_lit("build"), string8_lit("Build"), &c)) {
      (void)vkr_editor_build_start(build, editor, profile->run_after_build,
                                   message, sizeof(message));
    }
    c = build_widget(width - 146, 42, 134, 28);
    vkr_editor_primary_style(&c, editor->heading_font);
    c.disabled = !valid || vkr_editor_build_busy(build);
    c.icon = VKR_UI_ICON_PLAY;
    c.icon_size_pt = 13.0f;
    if (vkr_ui_button(ui, string8_lit("build.run"),
                      string8_lit("Build and Run"), &c)) {
      (void)vkr_editor_build_start(build, editor, true_v, message,
                                   sizeof(message));
    }
    if (build->package_bytes) {
      c = build_widget(110, 46, width - 390, 22);
      c.style.font_size_pt = theme->font_caption;
      c.style.text_color = theme->text_secondary;
      vkr_ui_label(ui, string8_lit("size"),
                   string8_create_formatted(
                       ui->frame_allocator, "Last package %.1f MiB",
                       (float64_t)build->package_bytes / (1024.0 * 1024.0)),
                   &c);
    }
    if (message[0]) {
      vkr_editor_toast(editor, VKR_UI_ICON_EXPORT, theme->accent_hover,
                       message);
    }
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

// =============================================================================
// Build tab and status strip
// =============================================================================

void vkr_editor_build_panel(VkrEditorBuild *build, VkrEditorUi *editor,
                            const VkrSampleUiFrame *frame, VkrUiRect rect) {
  VkrUiSystem *ui = frame->ui;
  const float32_t width = rect.width / ui->content_scale;
  if (!build || width < 200) {
    return;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t log_lines = (float32_t)build->log_length / 60.0f + 4.0f;
  const float32_t content_height =
      70.0f + 26.0f * BUILD_STAGE_COUNT + 18.0f * log_lines;
  const float32_t height = rect.height / ui->content_scale;
  if (!ui->mouse_captured && ui->mouse_input_layer == ui->input_layer &&
      ui->mouse_x >= rect.x && ui->mouse_x < rect.x + rect.width &&
      ui->mouse_y >= rect.y && ui->mouse_y < rect.y + rect.height) {
    build->panel_scroll -= ui->mouse_wheel * 40;
  }
  build->panel_scroll =
      vkr_clamp_f32(build->panel_scroll, 0, Max(0.0f, content_height - height));
  VkrUiPanelConfig scroll = vkr_ui_panel_config_default();
  const VkrUiTrack track = {.value = content_height, .unit = VKR_UI_TRACK_PX};
  scroll.rows = &track;
  scroll.row_count = 1;
  scroll.clip_children = true_v;
  if (!vkr_ui_scroll_area_begin(ui, string8_lit("build.panel"), &scroll)) {
    return;
  }
  (void)vkr_ui_scroll_area_offset(ui, &build->panel_scroll);
  float32_t y = 8;
  const char *headline =
      build->phase == BUILD_PREFLIGHT ? "Waiting for unsaved edits"
      : build->phase == BUILD_RUNNING ? "Building"
      : build->result[0]              ? build->result
                         : "No build yet. Build > Build packages the project.";
  VkrUiWidgetConfig c = build_widget(10, y, width - 20, 24);
  c.text.font = editor->heading_font;
  c.icon = build->phase == BUILD_RUNNING ? VKR_UI_ICON_SPINNER
           : build->phase == BUILD_FINISHED && !build->succeeded
               ? VKR_UI_ICON_WARNING_FILL
           : build->succeeded ? VKR_UI_ICON_CHECK_CIRCLE
                              : VKR_UI_ICON_EXPORT;
  c.icon_size_pt = 14.0f;
  c.icon_color = build->phase == BUILD_FINISHED && !build->succeeded
                     ? theme->warning
                 : build->succeeded ? theme->success
                                    : theme->accent_hover;
  vkr_ui_label(ui, string8_lit("headline"), build_text(headline), &c);
  y += 30;
  /* Actions for the finished package. */
  const char *actions[] = {"Open Folder", "Run", "Copy Log"};
  const VkrUiIcon icons[] = {VKR_UI_ICON_FOLDER, VKR_UI_ICON_PLAY,
                             VKR_UI_ICON_COPY};
  for (uint32_t i = 0; i < ArrayCount(actions); ++i) {
    c = build_widget(10 + i * 118, y, 112, 26);
    vkr_editor_action_style(&c, editor->heading_font);
    c.icon = icons[i];
    c.icon_size_pt = 13.0f;
    c.disabled = i == 2u   ? !build->log_length
                 : i == 1u ? !build->executable[0] || build->game != NULL ||
                                 vkr_editor_build_busy(build)
                           : !build->output[0];
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("action"), build_text(actions[i]), &c)) {
      char message[256] = {0};
      if (i == 0u) {
        (void)vkr_editor_build_open_last(build, message, sizeof(message));
      } else if (i == 1u) {
        (void)build_run_game(
            build, vkr_editor_projects_workspace_root(editor->projects),
            message, sizeof(message));
      } else {
        (void)vkr_platform_clipboard_write_text(build->log, build->log_length);
        snprintf(message, sizeof(message), "Copied the build log");
      }
      vkr_editor_toast(editor, icons[i], theme->accent_hover, message);
    }
    (void)vkr_ui_pop_id(ui);
  }
  if (build->phase == BUILD_RUNNING || build->phase == BUILD_PREFLIGHT) {
    c = build_widget(10 + 3 * 118, y, 90, 26);
    vkr_editor_action_style(&c, editor->heading_font);
    c.icon = VKR_UI_ICON_STOP;
    c.icon_size_pt = 13.0f;
    if (vkr_ui_button(ui, string8_lit("cancel"), string8_lit("Cancel"), &c)) {
      vkr_editor_build_cancel(build, editor);
    }
  }
  y += 34;
  for (uint32_t i = 0; i < BUILD_STAGE_COUNT; ++i) {
    const BuildStage *stage = &build->stages[i];
    c = build_widget(10, y, width - 110, 22);
    c.style.text_color = stage->status == BUILD_STAGE_PENDING
                             ? theme->text_disabled
                             : theme->text;
    c.icon = stage->status == BUILD_STAGE_DONE      ? VKR_UI_ICON_CHECK_CIRCLE
             : stage->status == BUILD_STAGE_FAILED  ? VKR_UI_ICON_WARNING_FILL
             : stage->status == BUILD_STAGE_RUNNING ? VKR_UI_ICON_SPINNER
                                                    : VKR_UI_ICON_CIRCLE;
    c.icon_color = stage->status == BUILD_STAGE_DONE     ? theme->success
                   : stage->status == BUILD_STAGE_FAILED ? theme->error
                                                         : theme->accent_hover;
    c.icon_size_pt = 12.0f;
    const bool8_t current =
        stage->status == BUILD_STAGE_RUNNING && build->detail[0];
    (void)vkr_ui_push_id_u64(ui, i);
    vkr_ui_label(ui, string8_lit("stage"),
                 current ? string8_create_formatted(
                               ui->frame_allocator, "%s  \xc2\xb7  %s",
                               build_stage_names[i], build->detail)
                         : build_text(build_stage_names[i]),
                 &c);
    if (stage->status == BUILD_STAGE_DONE ||
        stage->status == BUILD_STAGE_FAILED) {
      c = build_widget(width - 96, y, 86, 22);
      c.style.text_color = theme->text_secondary;
      c.style.font_size_pt = theme->font_caption;
      vkr_ui_label(ui, string8_lit("seconds"),
                   string8_create_formatted(ui->frame_allocator, "%.2f s",
                                            stage->seconds),
                   &c);
    }
    (void)vkr_ui_pop_id(ui);
    y += 26;
  }
  y += 6;
  if (build->log_length) {
    c = build_widget(10, y, width - 20, 18.0f * log_lines);
    c.style.min_size_pt.y = 20;
    c.style.max_size_pt.y = 18.0f * log_lines;
    c.style.font_size_pt = theme->font_caption;
    c.style.text_color = theme->text_secondary;
    c.text.font = editor->mono_font;
    c.text.layout.word_wrap = true_v;
    c.text.layout.max_width = width - 28;
    vkr_ui_label(ui, string8_lit("log"),
                 string8_create(build->log, build->log_length), &c);
  }
  vkr_ui_scroll_area_end(ui);
}

void vkr_editor_build_status_build(VkrEditorBuild *build, VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame) {
  if (!vkr_editor_build_busy(build)) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t target_width =
      (float32_t)ui->target_width / ui->content_scale;
  const float32_t target_height =
      (float32_t)ui->target_height / ui->content_scale;
  const float32_t width = Min(460.0f, target_width - 24.0f);
  const Vec2 origin = {(target_width - width) * 0.5f, target_height - 58.0f};
  const VkrUiTrack columns[] = {
      {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
      {.value = 84.0f, .unit = VKR_UI_TRACK_PX},
  };
  const VkrUiTrack row = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig strip = vkr_ui_panel_config_default();
  strip.placement = (VkrUiPlacement){
      .column_span = 1,
      .row_span = 1,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
      .margin_pt = {origin.y, 0, 0, origin.x},
  };
  strip.columns = columns;
  strip.column_count = ArrayCount(columns);
  strip.rows = &row;
  strip.row_count = 1;
  strip.style = vkr_editor_glass_style();
  strip.style.min_size_pt = strip.style.max_size_pt = (Vec2){width, 40.0f};
  strip.style.padding_pt = (VkrUiEdges){6, 10, 6, 10};
  if (!vkr_ui_panel_begin(ui, string8_lit("build.status"), &strip)) {
    return;
  }
  const char *stage =
      build->phase == BUILD_PREFLIGHT    ? "Waiting for unsaved edits"
      : build->stage < BUILD_STAGE_COUNT ? build_stage_names[build->stage]
                                         : "Queued behind another Bakery job";
  VkrUiWidgetConfig c = vkr_ui_widget_config_default();
  c.placement = (VkrUiPlacement){.column = 0,
                                 .column_span = 1,
                                 .row_span = 1,
                                 .justify = VKR_UI_ALIGN_START,
                                 .align = VKR_UI_ALIGN_CENTER};
  c.style.font_size_pt = theme->font_body;
  c.style.text_color = theme->text;
  c.icon = VKR_UI_ICON_SPINNER;
  c.icon_size_pt = 13.0f;
  c.icon_color = theme->accent_hover;
  const uint32_t done = build->stage < BUILD_STAGE_COUNT ? build->stage : 0u;
  vkr_ui_label(ui, string8_lit("stage"),
               string8_create_formatted(
                   ui->frame_allocator, "Build %u/%u  %s%s%s", done + 1u,
                   BUILD_STAGE_COUNT, stage,
                   build->detail[0] ? " \xc2\xb7 " : "", build->detail),
               &c);
  VkrUiWidgetConfig cancel = vkr_ui_widget_config_default();
  cancel.placement = (VkrUiPlacement){.column = 1,
                                      .column_span = 1,
                                      .row_span = 1,
                                      .justify = VKR_UI_ALIGN_END,
                                      .align = VKR_UI_ALIGN_CENTER};
  vkr_editor_action_style(&cancel, editor->heading_font);
  cancel.style.min_size_pt = cancel.style.max_size_pt = (Vec2){78, 26};
  if (vkr_ui_button(ui, string8_lit("cancel"), string8_lit("Cancel"),
                    &cancel)) {
    vkr_editor_build_cancel(build, editor);
  }
  (void)vkr_ui_panel_end(ui);
}
