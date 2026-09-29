/* `vkr_bakery bundle <project directory> [--profile <name>] [--out <dir>]`
 * (docs/proposals/project-packaging.md): packages a managed project as a
 * standalone game directory that runs from any location:
 *
 *   <out>/<executable>[.exe]      the profile's player template
 *   <out>/bundle.json             version 2, with a `game` object
 *   <out>/shaders/<backend>/...   the target backend's catalog only
 *   <out>/content/game.vkpak      `project/...` and `editor/...` identities
 *   <out>/content/engine.vkpak    `assets/...`: render graph, runtime fonts
 *
 * Each stage is a `start`/`done` pair in the event stream with coded
 * diagnostics: Validate, Finalize, Bake, Lower, Pack, Stage runtime, Verify
 * and report. Project jobs run as child `vkr_bakery project` processes, so
 * workspace publication keeps its owners: scene jobs publish their scenes,
 * and the finalized project inventory is only lowered against, since the
 * editor publishes project.json. The package is assembled in `<out>.staging`
 * and replaces `<out>` only after verification, so a failed or cancelled
 * build keeps the previous package. */

#include "vkr_bakery_bundle.h"

#include "filesystem/vkr_vfs.h"
#include "platform/vkr_platform.h"
#include "project/vkr_project_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#define VKR_PACKAGE_MAX_SCENES 128u
#define VKR_PACKAGE_JSON_LIMIT (64ull * 1024ull * 1024ull)
#define VKR_PACKAGE_LARGEST 16u

#if defined(_WIN32)
#define VKR_PACKAGE_PLATFORM "windows-x64"
#define VKR_PACKAGE_BACKEND "vulkan"
#define VKR_PACKAGE_EXECUTABLE_SUFFIX ".exe"
#elif defined(__APPLE__) && defined(__aarch64__)
#define VKR_PACKAGE_PLATFORM "macos-arm64"
#define VKR_PACKAGE_BACKEND "metal"
#define VKR_PACKAGE_EXECUTABLE_SUFFIX ""
#elif defined(__APPLE__)
#define VKR_PACKAGE_PLATFORM "macos-x64"
#define VKR_PACKAGE_BACKEND "metal"
#define VKR_PACKAGE_EXECUTABLE_SUFFIX ""
#else
#define VKR_PACKAGE_PLATFORM "linux-x64"
#define VKR_PACKAGE_BACKEND "vulkan"
#define VKR_PACKAGE_EXECUTABLE_SUFFIX ""
#endif

typedef struct VkrPackageScene {
  const char *id;
  const char *name;
  const char *manifest; /* Absolute scene.json. */
  const char *identity; /* project/scenes/<id>/runtime.scene.json */
  const char *overlay;  /* project/scenes/<id>/runtime.editor.json */
  int64_t preview_assets;
  bool8_t has_player;
} VkrPackageScene;

typedef struct VkrPackage {
  VkrBakeryCli *cli;
  Arena *arena;
  char workspace[VKR_BAKERY_PATH_CAPACITY]; /* The `.vkreditor` directory. */
  char workspace_directory[VKR_BAKERY_PATH_CAPACITY];
  char project_root[VKR_BAKERY_PATH_CAPACITY];
  char project_path[VKR_BAKERY_PATH_CAPACITY];
  char template_root[VKR_BAKERY_PATH_CAPACITY];
  char engine_root[VKR_BAKERY_PATH_CAPACITY];
  char shaders[VKR_BAKERY_PATH_CAPACITY];
  char player[VKR_BAKERY_PATH_CAPACITY];
  char editor_bundle[VKR_BAKERY_PATH_CAPACITY];
  char out[VKR_BAKERY_PATH_CAPACITY];
  char staging[VKR_BAKERY_PATH_CAPACITY];
  char work[VKR_BAKERY_PATH_CAPACITY];
  char content[VKR_BAKERY_PATH_CAPACITY]; /* Staged documents by identity. */
  char report_path[VKR_BAKERY_PATH_CAPACITY];
  const VkrBakeryJson *project;
  const VkrBakeryJson *game;
  const VkrBakeryJson *profile;
  const VkrBakeryJson *template_description;
  const char *project_name;
  const char *profile_name;
  const char *config;
  const char *executable;
  VkrPackageScene scenes[VKR_PACKAGE_MAX_SCENES];
  uint32_t scene_count;
  uint32_t startup; /* Scene index, or UINT32_MAX for the World alone. */
  bool8_t bake_lighting;
  /* The inventory a Finalize stage returned; NULL lowers against
     project.json. */
  VkrBakeryJson *project_assets;
  int64_t project_preview_assets;
  const char *world;
  const char *world_overlay;
  VkrBakeryJson *fonts;    /* name -> config identity */
  VkrBakeryJson *warnings; /* Array of strings. */
  VkrBakeryJson *stages;   /* Report records. */
  VkrBakeryJson *summary;  /* Pack statistics for the report. */
  uint32_t job_count;
  uint32_t stage_id;
  const char *stage_name;
  float64_t stage_started;
  /* A running child job's progress file, polled while waiting on it. */
  const char *progress_path;
  float64_t progress_polled;
} VkrPackage;

// =============================================================================
// Events and documents
// =============================================================================

vkr_internal bool8_t vkr_package_cancelled(void) {
  return vkr_atomic_bool_load(&vkr_bakery_cancel_requested,
                              VKR_MEMORY_ORDER_RELAXED)
             ? true_v
             : false_v;
}

vkr_internal bool8_t vkr_package_fail(VkrPackage *package, const char *source,
                                      const char *format, ...) {
  char message[1024];
  va_list arguments;
  va_start(arguments, format);
  (void)vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  vkr_bakery_event_diag(package->stage_id, VKR_BAKERY_DIAG_BUNDLE_FAILED,
                        source ? source : package->project_path, 0u, 0u,
                        message, NULL);
  return false_v;
}

vkr_internal void vkr_package_warn(VkrPackage *package, const char *format,
                                   ...) {
  char message[1024];
  va_list arguments;
  va_start(arguments, format);
  (void)vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  vkr_bakery_json_append(package->warnings,
                         vkr_bakery_json_cstr(package->arena, message));
  vkr_bakery_event_log(package->stage_id, "warn", message, strlen(message));
}

vkr_internal void vkr_package_stage_begin(VkrPackage *package,
                                          const char *name) {
  package->stage_id += 1u;
  package->stage_name = name;
  package->stage_started = vkr_bakery_monotonic_seconds();
  vkr_bakery_event_start(package->stage_id, "", "package", name, name, 0u);
}

vkr_internal bool8_t vkr_package_stage_end(VkrPackage *package, bool8_t ok,
                                           const char *detail) {
  const float64_t seconds =
      vkr_bakery_monotonic_seconds() - package->stage_started;
  const bool8_t cancelled = !ok && vkr_package_cancelled();
  const VkrBakeryDoneEvent done = {
      .id = package->stage_id,
      .status = ok          ? VKR_BAKERY_ACTION_OK
                : cancelled ? VKR_BAKERY_ACTION_CANCELLED
                            : VKR_BAKERY_ACTION_FAILED,
      .wall_ms = (uint64_t)(seconds * 1000.0),
      .peak_rss_source = "none",
      .source = package->stage_name,
      .producer = "package",
  };
  vkr_bakery_event_done(&done);
  Arena *arena = package->arena;
  VkrBakeryJson *record = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, record, "name",
                      vkr_bakery_json_cstr(arena, package->stage_name));
  vkr_bakery_json_set(arena, record, "status",
                      vkr_bakery_json_cstr(arena, ok          ? "complete"
                                                  : cancelled ? "cancelled"
                                                              : "failed"));
  vkr_bakery_json_set(arena, record, "seconds",
                      vkr_bakery_json_float(arena, seconds));
  if (detail) {
    vkr_bakery_json_set(arena, record, "detail",
                        vkr_bakery_json_cstr(arena, detail));
  }
  vkr_bakery_json_append(package->stages, record);
  return ok;
}

vkr_internal VkrBakeryJson *
vkr_package_read_json(VkrPackage *package, const char *path, bool8_t required) {
  uint8_t *data = NULL;
  uint64_t length = 0u;
  VkrBakeryJsonError error = {0};
  VkrBakeryJson *value = NULL;
  if (vkr_bakery_read_file(path, VKR_PACKAGE_JSON_LIMIT, &data, &length)) {
    value = vkr_bakery_json_parse(package->arena, data, length, 256u, &error);
  }
  free(data);
  if (!value && required) {
    vkr_bakery_event_diag(
        package->stage_id, VKR_BAKERY_DIAG_REC_UNREADABLE, path, error.line,
        error.column, error.message[0] ? error.message : "cannot read", NULL);
  }
  return value;
}

vkr_internal bool8_t vkr_package_write_json(VkrPackage *package,
                                            const char *path,
                                            const VkrBakeryJson *value) {
  String8 text = {0};
  char directory[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  if (!vkr_bakery_json_write(package->arena, value, VKR_BAKERY_JSON_PRETTY,
                             &text) ||
      !vkr_bakery_make_directories(directory) ||
      !vkr_bakery_write_file_atomic(path, text.str, text.length)) {
    return vkr_package_fail(package, path, "cannot write %s", path);
  }
  return true_v;
}

vkr_internal const char *vkr_package_text(VkrPackage *package,
                                          const VkrBakeryJson *object,
                                          const char *key) {
  return vkr_bakery_json_cstr_value(package->arena,
                                    vkr_bakery_json_get(object, key));
}

vkr_internal const char *vkr_package_printf(VkrPackage *package,
                                            const char *format, ...) {
  char text[VKR_BAKERY_PATH_CAPACITY];
  va_list arguments;
  va_start(arguments, format);
  (void)vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  return vkr_bakery_json_cstr_value(package->arena,
                                    vkr_bakery_json_cstr(package->arena, text));
}

// =============================================================================
// Child project jobs
// =============================================================================

/* Forwards the running job's progress document as progress events at most
   four times a second, then answers the cancellation query. */
vkr_internal bool8_t vkr_package_poll_job(void *context) {
  VkrPackage *package = context;
  const float64_t now = vkr_bakery_monotonic_seconds();
  if (package->progress_path && now - package->progress_polled >= 0.25) {
    package->progress_polled = now;
    uint8_t *data = NULL;
    uint64_t length = 0u;
    if (vkr_bakery_read_file(package->progress_path, KB(64), &data, &length)) {
      const uint64_t mark = arena_pos(package->arena);
      const VkrBakeryJson *progress =
          vkr_bakery_json_parse(package->arena, data, length, 8u, NULL);
      float64_t fraction = -1.0;
      String8 stage = {0};
      (void)vkr_bakery_json_get_number(progress, "progress", &fraction);
      (void)vkr_bakery_json_get_string(progress, "stage", &stage);
      char detail[256];
      (void)snprintf(detail, sizeof(detail), "%.*s", (int)stage.length,
                     stage.str);
      vkr_bakery_event_progress(package->stage_id, fraction, detail);
      arena_reset_to(package->arena, mark, ARENA_MEMORY_TAG_STRING);
    }
    free(data);
  }
  return vkr_package_cancelled();
}

/* The request fields every project job of this package shares. */
vkr_internal VkrBakeryJson *vkr_package_request(VkrPackage *package,
                                                const char *operation,
                                                const VkrPackageScene *scene,
                                                bool8_t portable) {
  Arena *arena = package->arena;
  VkrBakeryJson *request = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, request, "version", vkr_bakery_json_int(arena, 1));
  vkr_bakery_json_set(arena, request, "operation",
                      vkr_bakery_json_cstr(arena, operation));
  vkr_bakery_json_set(arena, request, "read_only",
                      vkr_bakery_json_bool(arena, portable));
  vkr_bakery_json_set(arena, request, "portable",
                      vkr_bakery_json_bool(arena, portable));
  vkr_bakery_json_set(arena, request, "workspace_root",
                      vkr_bakery_json_cstr(arena, package->workspace));
  vkr_bakery_json_set(arena, request, "project_path",
                      vkr_bakery_json_cstr(arena, package->project_path));
  if (scene) {
    vkr_bakery_json_set(arena, request, "scene_id",
                        vkr_bakery_json_cstr(arena, scene->id));
    vkr_bakery_json_set(arena, request, "scene_path",
                        vkr_bakery_json_cstr(arena, scene->manifest));
  }
  if (portable && package->project_assets) {
    vkr_bakery_json_set(arena, request, "project_assets",
                        package->project_assets);
  }
  /* A shipping package finalizes with the final encoder; a development
     package with the fast one the editor uses. */
  vkr_bakery_json_set(arena, request, "texture_tier",
                      vkr_bakery_json_cstr(arena, "final"));
  vkr_bakery_json_set(
      arena, request, "texture_encode_speed",
      vkr_bakery_json_cstr(
          arena, strcmp(package->config, "shipping") == 0 ? "final" : "fast"));
  return request;
}

/* Runs one `vkr_bakery project` job; returns its result, or NULL after
   reporting the job's error. Its request, result and log stay in the work
   directory until the package is published. */
vkr_internal VkrBakeryJson *vkr_package_run_job(VkrPackage *package,
                                                VkrBakeryJson *request,
                                                const char *label) {
  Arena *arena = package->arena;
  const char *directory = vkr_package_printf(
      package, "%s/jobs/%u", package->work, package->job_count++);
  const char *request_path =
      vkr_package_printf(package, "%s/request.json", directory);
  const char *result_path =
      vkr_package_printf(package, "%s/result.json", directory);
  const char *log_path = vkr_package_printf(package, "%s/log.txt", directory);
  if (!vkr_bakery_json_get(request, "runtime_directory")) {
    vkr_bakery_json_set(arena, request, "runtime_directory",
                        vkr_bakery_json_cstr(arena, directory));
  }
  if (!vkr_package_write_json(package, request_path, request)) {
    return NULL;
  }
  const char *arguments[] = {"project",
                             "--request",
                             request_path,
                             "--result",
                             result_path,
                             "--cache",
                             package->cli->config.cache_dir};
  const VkrPlatformProcessConfig config = {
      .executable = package->cli->config.self_path,
      .arguments = arguments,
      .argument_count = ArrayCount(arguments),
      .stdout_path = log_path,
      .stderr_path = log_path,
      .termination_grace_ms = 5000u,
      .terminate_process_tree = true_v,
      .hidden = true_v,
      .is_cancelled = vkr_package_poll_job,
      .cancel_context = package,
  };
  package->progress_path =
      vkr_package_printf(package, "%s.progress.json", result_path);
  vkr_bakery_event_progress(package->stage_id, -1.0, label);
  int32_t code = -1;
  bool8_t timed_out = false_v;
  const bool8_t ran = vkr_platform_process_run(&config, &code, &timed_out);
  package->progress_path = NULL;
  if (vkr_package_cancelled()) {
    vkr_package_fail(package, NULL, "%s was cancelled", label);
    return NULL;
  }
  if (!ran) {
    vkr_package_fail(package, NULL, "%s could not start %s", label,
                     package->cli->config.self_path);
    return NULL;
  }
  VkrBakeryJson *result = vkr_package_read_json(package, result_path, false_v);
  if (code != 0 || !result ||
      !vkr_bakery_json_is_string(vkr_bakery_json_get(result, "status"),
                                 "complete")) {
    const char *error = vkr_package_text(package, result, "error");
    vkr_package_fail(package, log_path, "%s failed: %s", label,
                     error && error[0] ? error : "see the job log");
    return NULL;
  }
  const VkrBakeryJson *warnings = vkr_bakery_json_get(result, "warnings");
  for (const VkrBakeryJson *warning = warnings ? warnings->first : NULL;
       warning; warning = warning->next) {
    if (warning->type == VKR_BAKERY_JSON_STRING) {
      vkr_package_warn(package, "%s: %.*s", label, (int)warning->string.length,
                       warning->string.str);
    }
  }
  return result;
}

// =============================================================================
// Validate
// =============================================================================

vkr_internal bool8_t vkr_package_executable_valid(const char *name) {
  const uint64_t length = name ? strlen(name) : 0u;
  if (!length || length > 64u || name[0] == ' ' || name[length - 1u] == ' ') {
    return false_v;
  }
  for (uint64_t i = 0u; i < length; ++i) {
    const char c = name[i];
    const bool8_t allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
                            c == '_';
    if (!allowed) {
      return false_v;
    }
  }
  return true_v;
}

/* The game.json a project without one has: the project's scenes, starting
   with the first, and one development profile for the host. */
vkr_internal VkrBakeryJson *vkr_package_default_game(VkrPackage *package) {
  Arena *arena = package->arena;
  char executable[65] = {0};
  uint32_t length = 0u;
  for (const char *c = package->project_name; *c && length < 64u; ++c) {
    const char text[2] = {*c, 0};
    if (vkr_package_executable_valid(text) && (*c != ' ' || length)) {
      executable[length++] = *c;
    }
  }
  while (length && executable[length - 1u] == ' ') {
    executable[--length] = 0;
  }
  VkrBakeryJson *game = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, game, "name",
                      vkr_bakery_json_cstr(arena, package->project_name));
  vkr_bakery_json_set(arena, game, "version",
                      vkr_bakery_json_cstr(arena, "0.1.0"));
  vkr_bakery_json_set(arena, game, "company", vkr_bakery_json_cstr(arena, ""));
  vkr_bakery_json_set(
      arena, game, "executable",
      vkr_bakery_json_cstr(arena, length ? executable : "Game"));
  VkrBakeryJson *scenes = vkr_bakery_json_array(arena);
  const VkrBakeryJson *listed = vkr_bakery_json_get(package->project, "scenes");
  for (const VkrBakeryJson *scene = listed ? listed->first : NULL; scene;
       scene = scene->next) {
    const VkrBakeryJson *id = vkr_bakery_json_get(scene, "id");
    if (id && id->type == VKR_BAKERY_JSON_STRING) {
      vkr_bakery_json_append(scenes, vkr_bakery_json_clone(arena, id));
    }
  }
  vkr_bakery_json_set(arena, game, "startup_scene",
                      scenes->first
                          ? vkr_bakery_json_clone(arena, scenes->first)
                          : vkr_bakery_json_cstr(arena, ""));
  vkr_bakery_json_set(arena, game, "scenes", scenes);
  VkrBakeryJson *window = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, window, "mode",
                      vkr_bakery_json_cstr(arena, "windowed"));
  vkr_bakery_json_set(arena, window, "width", vkr_bakery_json_int(arena, 1600));
  vkr_bakery_json_set(arena, window, "height", vkr_bakery_json_int(arena, 900));
  vkr_bakery_json_set(arena, game, "window", window);
  VkrBakeryJson *graphics = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, graphics, "version",
                      vkr_bakery_json_int(arena, 1));
  vkr_bakery_json_set(arena, game, "graphics", graphics);

  VkrBakeryJson *profile = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, profile, "name",
                      vkr_bakery_json_cstr(arena, package->project_name));
  vkr_bakery_json_set(arena, profile, "platform",
                      vkr_bakery_json_cstr(arena, "host"));
  vkr_bakery_json_set(arena, profile, "config",
                      vkr_bakery_json_cstr(arena, "development"));
  vkr_bakery_json_set(arena, profile, "output",
                      vkr_bakery_json_cstr(arena, ""));
  vkr_bakery_json_set(arena, profile, "include", vkr_bakery_json_array(arena));
  vkr_bakery_json_set(arena, profile, "bake_lighting",
                      vkr_bakery_json_bool(arena, false_v));
  VkrBakeryJson *profiles = vkr_bakery_json_array(arena);
  vkr_bakery_json_append(profiles, profile);

  VkrBakeryJson *document = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, document, "version",
                      vkr_bakery_json_int(arena, 1));
  vkr_bakery_json_set(arena, document, "game", game);
  vkr_bakery_json_set(arena, document, "profiles", profiles);
  return document;
}

vkr_internal const VkrBakeryJson *
vkr_package_project_scene(const VkrPackage *package, const char *id) {
  const VkrBakeryJson *scenes = vkr_bakery_json_get(package->project, "scenes");
  for (const VkrBakeryJson *scene = scenes ? scenes->first : NULL; scene;
       scene = scene->next) {
    if (vkr_bakery_json_is_string(vkr_bakery_json_get(scene, "id"), id)) {
      return scene;
    }
  }
  return NULL;
}

vkr_internal bool8_t vkr_package_game_settings(VkrPackage *package,
                                               const char *game_path) {
  const VkrBakeryJson *game = vkr_bakery_json_get(package->game, "game");
  int64_t version = 0;
  if (!vkr_bakery_json_get_int(package->game, "version", &version) ||
      version != 1 || !game || game->type != VKR_BAKERY_JSON_OBJECT) {
    return vkr_package_fail(package, game_path,
                            "game.json needs version 1 and a game object");
  }
  String8 name = {0};
  if (!vkr_bakery_json_get_string(game, "name", &name) || !name.length) {
    return vkr_package_fail(package, game_path, "the game needs a name");
  }
  package->executable = vkr_package_text(package, game, "executable");
  if (!vkr_package_executable_valid(package->executable)) {
    return vkr_package_fail(
        package, game_path,
        "the executable name must be 1 to 64 letters, digits, spaces, '-' "
        "or '_', without a separator or extension");
  }
  const VkrBakeryJson *window = vkr_bakery_json_get(game, "window");
  int64_t width = 0;
  int64_t height = 0;
  if (!window || window->type != VKR_BAKERY_JSON_OBJECT ||
      !vkr_bakery_json_is_string(vkr_bakery_json_get(window, "mode"),
                                 "windowed") ||
      !vkr_bakery_json_get_int(window, "width", &width) ||
      !vkr_bakery_json_get_int(window, "height", &height) || width < 320 ||
      height < 240 || width > 16384 || height > 16384) {
    return vkr_package_fail(package, game_path,
                            "the window must be windowed, 320x240 to "
                            "16384x16384; other modes are not supported yet");
  }
  const VkrBakeryJson *graphics = vkr_bakery_json_get(game, "graphics");
  if (graphics && graphics->type != VKR_BAKERY_JSON_OBJECT) {
    return vkr_package_fail(package, game_path,
                            "graphics must be a Graphics settings object");
  }

  /* Included scenes in their listed order; the World is always included. */
  const VkrBakeryJson *scenes = vkr_bakery_json_get(game, "scenes");
  if (!scenes || scenes->type != VKR_BAKERY_JSON_ARRAY ||
      scenes->count > VKR_PACKAGE_MAX_SCENES) {
    return vkr_package_fail(package, game_path,
                            "scenes must list at most %u scene ids",
                            VKR_PACKAGE_MAX_SCENES);
  }
  const char *startup = vkr_package_text(package, game, "startup_scene");
  package->startup = UINT32_MAX;
  for (const VkrBakeryJson *entry = scenes->first; entry; entry = entry->next) {
    const char *id = vkr_bakery_json_cstr_value(package->arena, entry);
    const VkrBakeryJson *listed =
        id ? vkr_package_project_scene(package, id) : NULL;
    if (!listed) {
      return vkr_package_fail(package, game_path,
                              "included scene %s is not in the project",
                              id ? id : "(invalid)");
    }
    for (uint32_t i = 0u; i < package->scene_count; ++i) {
      if (strcmp(package->scenes[i].id, id) == 0) {
        return vkr_package_fail(package, game_path,
                                "scene %s is included twice", id);
      }
    }
    char manifest[VKR_BAKERY_PATH_CAPACITY];
    const char *relative = vkr_package_text(package, listed, "path");
    if (!relative || !vkr_bakery_path_join(manifest, sizeof(manifest),
                                           package->project_root, relative)) {
      return vkr_package_fail(package, game_path, "scene %s has no path", id);
    }
    VkrPackageScene *scene = &package->scenes[package->scene_count];
    *scene = (VkrPackageScene){
        .id = id,
        .name = vkr_package_text(package, listed, "name"),
        .manifest = vkr_package_printf(package, "%s", manifest),
        .identity = vkr_package_printf(
            package, "project/scenes/%s/runtime.scene.json", id),
        .overlay = vkr_package_printf(
            package, "project/scenes/%s/runtime.editor.json", id),
    };
    if (startup && strcmp(startup, id) == 0) {
      package->startup = package->scene_count;
    }
    package->scene_count += 1u;
  }
  if (package->scene_count && package->startup == UINT32_MAX) {
    return vkr_package_fail(package, game_path,
                            "the startup scene must be one of the included "
                            "scenes");
  }
  return true_v;
}

vkr_internal bool8_t vkr_package_select_profile(VkrPackage *package,
                                                const char *game_path) {
  const VkrBakeryJson *profiles =
      vkr_bakery_json_get(package->game, "profiles");
  for (const VkrBakeryJson *profile = profiles ? profiles->first : NULL;
       profile; profile = profile->next) {
    const char *name = vkr_package_text(package, profile, "name");
    if (name &&
        (!package->cli->profile || strcmp(name, package->cli->profile) == 0)) {
      package->profile = profile;
      package->profile_name = name;
      break;
    }
  }
  if (!package->profile) {
    return vkr_package_fail(package, game_path, "game.json has no profile %s",
                            package->cli->profile ? package->cli->profile : "");
  }
  const char *platform =
      vkr_package_text(package, package->profile, "platform");
  if (!platform || (strcmp(platform, "host") != 0 &&
                    strcmp(platform, VKR_PACKAGE_PLATFORM) != 0)) {
    return vkr_package_fail(package, game_path,
                            "profile %s targets %s; each platform packages "
                            "on its own host (%s)",
                            package->profile_name, platform ? platform : "none",
                            VKR_PACKAGE_PLATFORM);
  }
  package->config = vkr_package_text(package, package->profile, "config");
  if (!package->config || (strcmp(package->config, "development") != 0 &&
                           strcmp(package->config, "shipping") != 0)) {
    return vkr_package_fail(package, game_path,
                            "profile %s: config must be development or "
                            "shipping",
                            package->profile_name);
  }
  const VkrBakeryJson *bake =
      vkr_bakery_json_get(package->profile, "bake_lighting");
  if (bake && bake->type != VKR_BAKERY_JSON_BOOL) {
    return vkr_package_fail(package, game_path,
                            "profile %s: bake_lighting must be boolean",
                            package->profile_name);
  }
  package->bake_lighting = bake && bake->boolean;
  const char *output =
      package->cli->out ? package->cli->out
                        : vkr_package_text(package, package->profile, "output");
  if (!output || !output[0]) {
    return vkr_package_fail(package, game_path,
                            "profile %s has no output folder; set one or pass "
                            "--out",
                            package->profile_name);
  }
  (void)vkr_project_resolve(output, false_v, package->out,
                            sizeof(package->out));
  return true_v;
}

vkr_internal bool8_t vkr_package_visit_any(void *context, const char *name,
                                           bool8_t is_directory) {
  (void)name;
  (void)is_directory;
  *(bool8_t *)context = false_v;
  return true_v;
}

/* The output replaces a previous package only: an existing directory must
   hold a bundle.json, and neither it nor its staging sibling may lie inside
   the workspace or the repository. */
vkr_internal bool8_t vkr_package_check_output(VkrPackage *package) {
  char repository[VKR_BAKERY_PATH_CAPACITY];
  (void)vkr_project_resolve(PROJECT_SOURCE_DIR, false_v, repository,
                            sizeof(repository));
  const char *roots[] = {package->workspace_directory, repository};
  for (uint32_t i = 0u; i < ArrayCount(roots); ++i) {
    if (vkr_project_is_relative_to(package->out, roots[i]) ||
        vkr_project_is_relative_to(roots[i], package->out)) {
      return vkr_package_fail(package, package->out,
                              "the output folder must be outside the "
                              "workspace and the repository (%s)",
                              roots[i]);
    }
  }
  char description[VKR_BAKERY_PATH_CAPACITY];
  (void)vkr_bakery_path_join(description, sizeof(description), package->out,
                             "bundle.json");
  bool8_t empty = true_v;
  if (vkr_bakery_is_directory(package->out) &&
      !vkr_bakery_is_file(description) &&
      (!vkr_bakery_list_directory(package->out, vkr_package_visit_any,
                                  &empty) ||
       !empty)) {
    return vkr_package_fail(package, package->out,
                            "the output folder exists and is not a package; "
                            "choose an empty or new folder");
  }
  if (vkr_bakery_is_file(package->out)) {
    return vkr_package_fail(package, package->out, "the output path is a file");
  }
  (void)snprintf(package->staging, sizeof(package->staging), "%s.staging",
                 package->out);
  (void)snprintf(package->work, sizeof(package->work), "%s/.work",
                 package->staging);
  (void)snprintf(package->content, sizeof(package->content), "%s/content",
                 package->work);
  /* A staging directory a failed or cancelled run left is removed now. */
  if (!vkr_bakery_remove_tree(package->staging) ||
      !vkr_bakery_make_directories(package->content)) {
    return vkr_package_fail(package, package->staging,
                            "cannot prepare the staging folder");
  }
  return true_v;
}

vkr_internal bool8_t vkr_package_check_template(VkrPackage *package) {
  const char *root = package->cli->player_template;
#if defined(VKR_BAKERY_PLAYER_TEMPLATE_DEFAULT)
  if (!root) {
    root = VKR_BAKERY_PLAYER_TEMPLATE_DEFAULT;
  }
#endif
  if (!root) {
    return vkr_package_fail(package, NULL,
                            "no player template; pass --template <dir>");
  }
  (void)vkr_project_resolve(root, false_v, package->template_root,
                            sizeof(package->template_root));
  char description[VKR_BAKERY_PATH_CAPACITY];
  (void)vkr_bakery_path_join(description, sizeof(description),
                             package->template_root, "template.json");
  package->template_description =
      vkr_package_read_json(package, description, false_v);
  int64_t version = 0;
  const VkrBakeryJson *players =
      vkr_bakery_json_get(package->template_description, "players");
  const char *player = vkr_package_text(package, players, package->config);
  const char *engine =
      vkr_package_text(package, package->template_description, "engine");
  const char *shaders =
      vkr_package_text(package, package->template_description, "shaders");
  if (!vkr_bakery_json_get_int(package->template_description, "version",
                               &version) ||
      version != 1 || !player || !engine || !shaders) {
    vkr_bakery_event_diag(package->stage_id,
                          VKR_BAKERY_DIAG_BUNDLE_MISSING_RUNTIME, description,
                          0u, 0u,
                          "the player template is missing or invalid; build "
                          "vkr_player",
                          NULL);
    return false_v;
  }
  char manifest[VKR_BAKERY_PATH_CAPACITY];
  const bool8_t found =
      vkr_bakery_path_join(package->player, sizeof(package->player),
                           package->template_root, player) &&
      vkr_bakery_path_join(package->engine_root, sizeof(package->engine_root),
                           package->template_root, engine) &&
      (vkr_bakery_path_is_absolute(shaders)
           ? (uint32_t)snprintf(package->shaders, sizeof(package->shaders),
                                "%s", shaders) < sizeof(package->shaders)
           : vkr_bakery_path_join(package->shaders, sizeof(package->shaders),
                                  package->template_root, shaders)) &&
      vkr_bakery_path_join(manifest, sizeof(manifest), package->shaders,
                           VKR_PACKAGE_BACKEND "/shader_manifest.json");
  if (!found || !vkr_bakery_is_file(package->player) ||
      !vkr_bakery_is_directory(package->engine_root) ||
      !vkr_bakery_is_file(manifest)) {
    vkr_bakery_event_diag(
        package->stage_id, VKR_BAKERY_DIAG_BUNDLE_MISSING_RUNTIME,
        package->player, 0u, 0u,
        "the player template lacks its executable, engine "
        "resources or " VKR_PACKAGE_BACKEND " shader catalog; build vkr_player",
        NULL);
    return false_v;
  }
  return true_v;
}

// =============================================================================
// Lower
// =============================================================================

/* Moves a job's lowered document to its identity below the staged content. */
vkr_internal bool8_t vkr_package_place(VkrPackage *package, const char *path,
                                       const char *identity) {
  char destination[VKR_BAKERY_PATH_CAPACITY];
  char directory[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bakery_path_join(destination, sizeof(destination), package->content,
                            identity)) {
    return false_v;
  }
  vkr_bakery_path_parent(directory, sizeof(directory), destination);
  if (!vkr_bakery_make_directories(directory) ||
      !vkr_bakery_rename(path, destination, true_v)) {
    return vkr_package_fail(package, path, "cannot stage %s", identity);
  }
  return true_v;
}

vkr_internal bool8_t vkr_package_has_player(const VkrBakeryJson *document) {
  const VkrBakeryJson *entities = vkr_bakery_json_get(document, "entities");
  for (const VkrBakeryJson *entity = entities ? entities->first : NULL; entity;
       entity = entity->next) {
    if (vkr_bakery_json_get(entity, "player")) {
      return true_v;
    }
  }
  return false_v;
}

vkr_internal void vkr_package_collect_fonts(VkrPackage *package,
                                            const VkrBakeryJson *result) {
  const VkrBakeryJson *fonts = vkr_bakery_json_get(result, "fonts");
  for (const VkrBakeryJson *font = fonts ? fonts->first : NULL; font;
       font = font->next) {
    const char *name = vkr_package_text(package, font, "name");
    const VkrBakeryJson *config = vkr_bakery_json_get(font, "config");
    if (name && config && config->type == VKR_BAKERY_JSON_STRING) {
      vkr_bakery_json_set(package->arena, package->fonts, name,
                          vkr_bakery_json_clone(package->arena, config));
    }
  }
}

/* Portable lowering of one included scene; its runtime document and overlay
   take their identities below the staged content. */
vkr_internal bool8_t vkr_package_lower_scene(VkrPackage *package,
                                             VkrPackageScene *scene) {
  VkrBakeryJson *request =
      vkr_package_request(package, "prepare_scene", scene, true_v);
  const char *label = vkr_package_printf(package, "Lowering %s",
                                         scene->name ? scene->name : scene->id);
  VkrBakeryJson *result = vkr_package_run_job(package, request, label);
  if (!result) {
    return false_v;
  }
  const char *runtime = vkr_package_text(package, result, "runtime_path");
  const char *overlay = vkr_package_text(package, result, "edit_path");
  const VkrBakeryJson *document =
      runtime ? vkr_package_read_json(package, runtime, true_v) : NULL;
  if (!document || !overlay ||
      !vkr_package_place(package, runtime, scene->identity) ||
      !vkr_package_place(package, overlay, scene->overlay)) {
    return vkr_package_fail(package, scene->manifest,
                            "lowering %s produced no runtime scene", scene->id);
  }
  scene->has_player = vkr_package_has_player(document);
  (void)vkr_bakery_json_get_int(result, "preview_assets",
                                &scene->preview_assets);
  vkr_package_collect_fonts(package, result);
  package->project_preview_assets =
      vkr_project_count_preview(vkr_bakery_json_get(result, "project_assets"));
  return true_v;
}

vkr_internal bool8_t vkr_package_lower_scenes(VkrPackage *package) {
  for (uint32_t i = 0u; i < package->scene_count; ++i) {
    if (!vkr_package_lower_scene(package, &package->scenes[i])) {
      return false_v;
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_package_lower_world(VkrPackage *package) {
  VkrBakeryJson *request =
      vkr_package_request(package, "package_world", NULL, true_v);
  VkrBakeryJson *result =
      vkr_package_run_job(package, request, "Packaging the World");
  if (!result) {
    return false_v;
  }
  const char *world = vkr_package_text(package, result, "world_path");
  const char *overlay = vkr_package_text(package, result, "edit_path");
  package->world = "";
  package->world_overlay = "";
  if (world && world[0]) {
    if (!vkr_package_place(package, world, "project/world.scene.json")) {
      return false_v;
    }
    package->world = "project/world.scene.json";
  }
  if (overlay && overlay[0]) {
    if (!vkr_package_place(package, overlay, "project/world.editor.json")) {
      return false_v;
    }
    package->world_overlay = "project/world.editor.json";
  }
  if (!package->scene_count) {
    package->project_preview_assets = vkr_project_count_preview(
        vkr_bakery_json_get(result, "project_assets"));
  }
  return true_v;
}

// =============================================================================
// Finalize and bake
// =============================================================================

/* Rebuilds preview- and deferred-tier assets at the final tier. A scene's
   job publishes its scene; the project inventory is only lowered against. */
vkr_internal bool8_t vkr_package_finalize(VkrPackage *package,
                                          bool8_t *out_changed) {
  uint32_t scenes = 0u;
  for (uint32_t i = 0u; i < package->scene_count; ++i) {
    VkrPackageScene *scene = &package->scenes[i];
    if (scene->preview_assets <= 0) {
      continue;
    }
    VkrBakeryJson *request =
        vkr_package_request(package, "finalize_textures", scene, false_v);
    if (!vkr_package_run_job(
            package, request,
            vkr_package_printf(package, "Finalizing %s",
                               scene->name ? scene->name : scene->id))) {
      return false_v;
    }
    scenes += 1u;
  }
  if (package->project_preview_assets > 0) {
    VkrBakeryJson *request =
        vkr_package_request(package, "finalize_project_assets", NULL, false_v);
    VkrBakeryJson *result =
        vkr_package_run_job(package, request, "Finalizing project assets");
    VkrBakeryJson *assets = vkr_bakery_json_get(result, "project_assets");
    if (!assets || assets->type != VKR_BAKERY_JSON_ARRAY) {
      return result ? vkr_package_fail(package, NULL,
                                       "finalizing returned no inventory")
                    : false_v;
    }
    package->project_assets = assets;
    vkr_package_warn(package,
                     "%lld project assets were finalized for this package; "
                     "the editor keeps them after it adopts the new "
                     "inventory",
                     (long long)package->project_preview_assets);
  }
  *out_changed = scenes > 0u || package->project_assets != NULL;
  return true_v;
}

vkr_internal bool8_t vkr_package_bake(VkrPackage *package) {
  Arena *arena = package->arena;
  for (uint32_t i = 0u; i < package->scene_count; ++i) {
    VkrPackageScene *scene = &package->scenes[i];
    VkrBakeryJson *request =
        vkr_package_request(package, "bake_scene", scene, false_v);
    VkrBakeryJson *bakes = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, bakes, "prepare_assets",
                        vkr_bakery_json_bool(arena, true_v));
    vkr_bakery_json_set(arena, bakes, "reflection",
                        vkr_bakery_json_bool(arena, true_v));
    vkr_bakery_json_set(arena, bakes, "diffuse",
                        vkr_bakery_json_bool(arena, true_v));
    vkr_bakery_json_set(arena, request, "bakes", bakes);
    if (!vkr_package_run_job(
            package, request,
            vkr_package_printf(package, "Baking %s",
                               scene->name ? scene->name : scene->id))) {
      return false_v;
    }
  }
  return true_v;
}

// =============================================================================
// Pack
// =============================================================================

vkr_internal bool8_t vkr_package_is_text(const char *identity) {
  static const char *const suffixes[] = {".json", ".mt", ".fontcfg"};
  const uint64_t length = strlen(identity);
  for (uint32_t i = 0u; i < ArrayCount(suffixes); ++i) {
    const uint64_t suffix = strlen(suffixes[i]);
    if (length >= suffix &&
        strcmp(identity + length - suffix, suffixes[i]) == 0) {
      return true_v;
    }
  }
  return false_v;
}

/* No archived document may name the workspace or the repository: the
   package runs where neither exists. */
vkr_internal bool8_t vkr_package_check_portable(VkrPackage *package,
                                                VkrBundle *bundle) {
  char repository[VKR_BAKERY_PATH_CAPACITY];
  (void)vkr_project_resolve(PROJECT_SOURCE_DIR, false_v, repository,
                            sizeof(repository));
  const char *roots[] = {package->workspace_directory, repository};
  bool8_t ok = true_v;
  for (uint32_t i = 0u; i < bundle->files.count; ++i) {
    const char *identity = bundle->files.items[i];
    char path[VKR_BAKERY_PATH_CAPACITY];
    uint8_t *data = NULL;
    uint64_t length = 0u;
    if (!vkr_package_is_text(identity) ||
        !vkr_bundle_source(bundle, identity, path, sizeof(path)) ||
        !vkr_bakery_read_file(path, VKR_PACKAGE_JSON_LIMIT, &data, &length)) {
      free(data);
      continue;
    }
    for (uint32_t r = 0u; r < ArrayCount(roots); ++r) {
      const uint64_t root_length = strlen(roots[r]);
      for (uint64_t c = 0u; root_length && c + root_length <= length; ++c) {
        if (MemCompare(data + c, roots[r], root_length) == 0) {
          ok = vkr_package_fail(package, identity,
                                "the document names %s, which the package "
                                "cannot hold",
                                roots[r]);
          break;
        }
      }
    }
    free(data);
  }
  return ok;
}

typedef struct VkrPackageSize {
  const char *identity;
  uint64_t size;
} VkrPackageSize;

vkr_internal int vkr_package_compare_size(const void *lhs, const void *rhs) {
  const VkrPackageSize *a = lhs;
  const VkrPackageSize *b = rhs;
  return a->size < b->size ? 1 : a->size > b->size ? -1 : 0;
}

vkr_internal const char *vkr_package_loader_name(uint32_t loader) {
  static const char *const names[] = {
      "raw",  "scene",     "mesh",      "texture", "material",
      "font", "animation", "collision", "volume",  "json"};
  return loader < ArrayCount(names) ? names[loader] : "raw";
}

/* Adds one closure root and closes the set over it; the files it reached
   first count toward `owner` in the report. */
vkr_internal void vkr_package_root(VkrBundle *bundle, const char *identity,
                                   bool8_t directory) {
  if (!identity || !identity[0]) {
    return;
  }
  if (directory) {
    vkr_bundle_add_directory(bundle, identity);
  } else {
    (void)vkr_bundle_add(bundle, identity, strlen(identity), true_v);
  }
  vkr_bundle_close(bundle);
}

/* A profile include: a '/'-separated path below the project, without empty,
   `.` or `..` names. */
vkr_internal bool8_t vkr_package_relative_valid(const char *relative) {
  if (!relative || !relative[0] || vkr_bakery_path_is_absolute(relative) ||
      strchr(relative, '\\')) {
    return false_v;
  }
  const char *part = relative;
  for (const char *c = relative;; ++c) {
    if (*c == '/' || *c == 0) {
      const uint64_t length = (uint64_t)(c - part);
      if (!length || (length == 1u && part[0] == '.') ||
          (length == 2u && part[0] == '.' && part[1] == '.')) {
        return false_v;
      }
      if (*c == 0) {
        return true_v;
      }
      part = c + 1;
    }
  }
}

/* Items borrow the closure set's strings; the report and bundle.json outlive
   the set, so they take arena copies. */
vkr_internal void vkr_package_keep_identities(VkrPackage *package,
                                              VkrBundleItem *items,
                                              uint32_t count) {
  for (uint32_t i = 0u; i < count; ++i) {
    items[i].identity = vkr_bakery_json_cstr_value(
        package->arena,
        vkr_bakery_json_cstr(package->arena, items[i].identity));
  }
}

typedef struct VkrPackageOwner {
  const char *name;
  uint32_t first; /* First file index the owner reached. */
} VkrPackageOwner;

vkr_internal bool8_t vkr_package_pack(VkrPackage *package,
                                      VkrBundleItem **out_items,
                                      uint32_t *out_count) {
  Arena *arena = package->arena;
  char engine_assets[VKR_BAKERY_PATH_CAPACITY];
  (void)vkr_bakery_path_join(engine_assets, sizeof(engine_assets),
                             package->engine_root, "assets");
  VkrBundle bundle = {
      .cli = package->cli,
      .arena = arena,
      .mounts = {{.prefix = "", .directory = package->content},
                 {.prefix = "project/", .directory = package->project_root},
                 {.prefix = "editor/", .directory = package->editor_bundle},
                 {.prefix = "assets/", .directory = engine_assets}},
      .mount_count = 4u,
  };
  VkrPackageOwner owners[VKR_PACKAGE_MAX_SCENES + 4u];
  uint32_t owner_count = 0u;
  owners[owner_count++] = (VkrPackageOwner){.name = "World", .first = 0u};
  vkr_package_root(&bundle, package->world, false_v);
  vkr_package_root(&bundle, package->world_overlay, false_v);
  for (uint32_t i = 0u; i < package->scene_count; ++i) {
    owners[owner_count++] = (VkrPackageOwner){.name = package->scenes[i].id,
                                              .first = bundle.files.count};
    vkr_package_root(&bundle, package->scenes[i].identity, false_v);
    vkr_package_root(&bundle, package->scenes[i].overlay, false_v);
  }
  owners[owner_count++] =
      (VkrPackageOwner){.name = "fonts", .first = bundle.files.count};
  for (const VkrBakeryJson *font = package->fonts->first; font;
       font = font->next) {
    vkr_package_root(&bundle, vkr_bakery_json_cstr_value(arena, font), false_v);
  }
  owners[owner_count++] =
      (VkrPackageOwner){.name = "include", .first = bundle.files.count};
  const VkrBakeryJson *include =
      vkr_bakery_json_get(package->profile, "include");
  for (const VkrBakeryJson *entry = include ? include->first : NULL; entry;
       entry = entry->next) {
    const char *relative = vkr_bakery_json_cstr_value(arena, entry);
    char path[VKR_BAKERY_PATH_CAPACITY];
    if (!vkr_package_relative_valid(relative)) {
      vkr_package_fail(package, NULL,
                       "profile include %s is not a project-relative path",
                       relative ? relative : "(invalid)");
      goto fail;
    }
    (void)vkr_bakery_path_join(path, sizeof(path), package->project_root,
                               relative);
    vkr_package_root(&bundle,
                     vkr_package_printf(package, "project/%s", relative),
                     vkr_bakery_is_directory(path));
  }
  owners[owner_count++] =
      (VkrPackageOwner){.name = "engine", .first = bundle.files.count};
  const VkrBakeryJson *engine =
      vkr_bakery_json_get(package->template_description, "engine_include");
  for (const VkrBakeryJson *entry = engine ? engine->first : NULL; entry;
       entry = entry->next) {
    vkr_package_root(&bundle, vkr_bakery_json_cstr_value(arena, entry),
                     false_v);
  }
  if (bundle.missing) {
    vkr_package_fail(package, NULL, "%u package inputs are missing",
                     bundle.missing);
    goto fail;
  }
  if (!vkr_package_check_portable(package, &bundle)) {
    goto fail;
  }

  /* Per-owner bytes in discovery order, before hashing sorts nothing. */
  VkrBundleItem *items = NULL;
  uint64_t input_bytes = 0u;
  if (!vkr_bundle_hash(&bundle, &items, &input_bytes)) {
    free(items);
    goto fail;
  }
  VkrBakeryJson *by_owner = vkr_bakery_json_object(arena);
  for (uint32_t o = 0u; o < owner_count; ++o) {
    const uint32_t end =
        o + 1u < owner_count ? owners[o + 1u].first : bundle.files.count;
    uint64_t bytes = 0u;
    for (uint32_t i = owners[o].first; i < end; ++i) {
      bytes += items[i].size;
    }
    vkr_bakery_json_set(arena, by_owner, owners[o].name,
                        vkr_bakery_json_int(arena, (int64_t)bytes));
  }
  uint64_t by_loader[VKR_PACK_LOADER_COUNT] = {0};
  VkrPackageSize *sizes =
      malloc((bundle.files.count ? bundle.files.count : 1u) * sizeof(*sizes));
  uint32_t fast_textures = 0u;
  for (uint32_t i = 0u; sizes && i < bundle.files.count; ++i) {
    const uint32_t loader = vkr_bundle_loader(items[i].identity);
    by_loader[loader < VKR_PACK_LOADER_COUNT ? loader : 0u] += items[i].size;
    sizes[i] =
        (VkrPackageSize){.identity = items[i].identity, .size = items[i].size};
    fast_textures += loader == VKR_PACK_LOADER_TEXTURE &&
                     (strstr(items[i].identity, "-fast.vkt") ||
                      strstr(items[i].identity, "-fast-preview.vkt"));
  }
  VkrBakeryJson *loaders = vkr_bakery_json_object(arena);
  for (uint32_t l = 0u; l < VKR_PACK_LOADER_COUNT; ++l) {
    if (by_loader[l]) {
      vkr_bakery_json_set(arena, loaders, vkr_package_loader_name(l),
                          vkr_bakery_json_int(arena, (int64_t)by_loader[l]));
    }
  }
  VkrBakeryJson *largest = vkr_bakery_json_array(arena);
  if (sizes) {
    qsort(sizes, bundle.files.count, sizeof(*sizes), vkr_package_compare_size);
    for (uint32_t i = 0u; i < bundle.files.count && i < VKR_PACKAGE_LARGEST;
         ++i) {
      VkrBakeryJson *entry = vkr_bakery_json_object(arena);
      vkr_bakery_json_set(arena, entry, "identity",
                          vkr_bakery_json_cstr(arena, sizes[i].identity));
      vkr_bakery_json_set(arena, entry, "bytes",
                          vkr_bakery_json_int(arena, (int64_t)sizes[i].size));
      vkr_bakery_json_append(largest, entry);
    }
  }
  free(sizes);
  if (fast_textures) {
    vkr_package_warn(package,
                     "%u textures use the fast encoder; they ship as encoded",
                     fast_textures);
  }
  package->summary = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, package->summary, "files",
                      vkr_bakery_json_int(arena, bundle.files.count));
  vkr_bakery_json_set(arena, package->summary, "content_bytes",
                      vkr_bakery_json_int(arena, (int64_t)input_bytes));
  vkr_bakery_json_set(arena, package->summary, "bytes_by_loader", loaders);
  vkr_bakery_json_set(arena, package->summary, "bytes_by_scene", by_owner);
  vkr_bakery_json_set(arena, package->summary, "largest", largest);
  vkr_bakery_json_set(arena, package->summary, "fast_textures",
                      vkr_bakery_json_int(arena, fast_textures));
  vkr_bakery_print("package %s: %u files, %.1f MiB\n", package->profile_name,
                   bundle.files.count,
                   (float64_t)input_bytes / (1024.0 * 1024.0));
  if (package->cli->config.dry_run) {
    for (uint32_t i = 0u; i < bundle.files.count; ++i) {
      vkr_bakery_print("  %s\n", bundle.files.items[i]);
    }
    vkr_package_keep_identities(package, items, bundle.files.count);
    *out_items = items;
    *out_count = bundle.files.count;
    vkr_bundle_free(&bundle);
    return true_v;
  }

  /* Engine resources and game content go to separate archives. */
  VkrBundleItem *game =
      malloc((bundle.files.count ? bundle.files.count : 1u) * sizeof(*game));
  VkrBundleItem *engine_items = malloc(
      (bundle.files.count ? bundle.files.count : 1u) * sizeof(*engine_items));
  uint32_t game_count = 0u;
  uint32_t engine_count = 0u;
  for (uint32_t i = 0u; game && engine_items && i < bundle.files.count; ++i) {
    if (strncmp(items[i].identity, "assets/", 7u) == 0) {
      engine_items[engine_count++] = items[i];
    } else {
      game[game_count++] = items[i];
    }
  }
  const struct {
    const char *name;
    VkrBundleItem *items;
    uint32_t count;
  } packs[] = {{"game.vkpak", game, game_count},
               {"engine.vkpak", engine_items, engine_count}};
  VkrBakeryJson *archives = vkr_bakery_json_array(arena);
  bool8_t ok = game && engine_items;
  char directory[VKR_BAKERY_PATH_CAPACITY];
  (void)vkr_bakery_path_join(directory, sizeof(directory), package->staging,
                             "content");
  ok = ok && vkr_bakery_make_directories(directory);
  for (uint32_t p = 0u; ok && p < ArrayCount(packs); ++p) {
    char path[VKR_BAKERY_PATH_CAPACITY];
    uint32_t chunks = 0u;
    uint64_t bytes = 0u;
    ok = vkr_bakery_path_join(path, sizeof(path), directory, packs[p].name) &&
         vkr_bundle_write_pack(&bundle, path, packs[p].items, packs[p].count,
                               &chunks, &bytes);
    if (!ok) {
      vkr_package_fail(package, path, "cannot write the archive");
      break;
    }
    VkrBakeryJson *record = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(
        arena, record, "path",
        vkr_bakery_json_cstr(
            arena, vkr_package_printf(package, "content/%s", packs[p].name)));
    vkr_bakery_json_set(arena, record, "entries",
                        vkr_bakery_json_int(arena, packs[p].count));
    vkr_bakery_json_set(arena, record, "chunks",
                        vkr_bakery_json_int(arena, chunks));
    vkr_bakery_json_set(arena, record, "bytes",
                        vkr_bakery_json_int(arena, (int64_t)bytes));
    vkr_bakery_json_append(archives, record);
    vkr_bakery_print("wrote %s: %u entries, %u chunks, %.1f MiB\n", path,
                     packs[p].count, chunks,
                     (float64_t)bytes / (1024.0 * 1024.0));
  }
  vkr_bakery_json_set(arena, package->summary, "archives", archives);
  /* The written archives sorted their items; the products map follows the
     identity order of both. */
  if (ok) {
    MemCopy(items, game, game_count * sizeof(*items));
    MemCopy(items + game_count, engine_items, engine_count * sizeof(*items));
  }
  free(game);
  free(engine_items);
  vkr_package_keep_identities(package, items, bundle.files.count);
  *out_items = items;
  *out_count = bundle.files.count;
  vkr_bundle_free(&bundle);
  return ok;

fail:
  vkr_bundle_free(&bundle);
  return false_v;
}

// =============================================================================
// Stage runtime, verify and report
// =============================================================================

vkr_internal VkrBakeryJson *vkr_package_game_object(VkrPackage *package) {
  Arena *arena = package->arena;
  const VkrBakeryJson *settings = vkr_bakery_json_get(package->game, "game");
  VkrBakeryJson *game = vkr_bakery_json_object(arena);
  static const char *const copied[] = {"name", "version", "company"};
  for (uint32_t i = 0u; i < ArrayCount(copied); ++i) {
    const char *text = vkr_package_text(package, settings, copied[i]);
    vkr_bakery_json_set(arena, game, copied[i],
                        vkr_bakery_json_cstr(arena, text ? text : ""));
  }
  const VkrPackageScene *startup = package->startup < package->scene_count
                                       ? &package->scenes[package->startup]
                                       : NULL;
  vkr_bakery_json_set(arena, game, "world",
                      vkr_bakery_json_cstr(arena, package->world));
  vkr_bakery_json_set(arena, game, "world_overlay",
                      vkr_bakery_json_cstr(arena, package->world_overlay));
  vkr_bakery_json_set(
      arena, game, "startup_scene",
      vkr_bakery_json_cstr(arena, startup ? startup->identity : ""));
  vkr_bakery_json_set(
      arena, game, "startup_overlay",
      vkr_bakery_json_cstr(arena, startup ? startup->overlay : ""));
  /* Without a player, the game starts where the author last viewed the
     startup scene: its editor viewport recall, without the selection. */
  const VkrBakeryJson *recall = vkr_bakery_json_get(
      vkr_bakery_json_get(
          vkr_bakery_json_get(package->project, "scene_editor_state"),
          startup ? startup->id : ""),
      "viewport");
  bool8_t camera_valid = false_v;
  if (recall &&
      vkr_bakery_json_get_bool(recall, "camera_valid", &camera_valid) &&
      camera_valid) {
    VkrBakeryJson *camera = vkr_bakery_json_clone(arena, recall);
    vkr_bakery_json_set(arena, camera, "selection_valid",
                        vkr_bakery_json_bool(arena, false_v));
    (void)vkr_bakery_json_remove(camera, "selection");
    vkr_bakery_json_set(arena, game, "startup_camera", camera);
  }
  VkrBakeryJson *scenes = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < package->scene_count; ++i) {
    const VkrPackageScene *scene = &package->scenes[i];
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, entry, "id",
                        vkr_bakery_json_cstr(arena, scene->id));
    vkr_bakery_json_set(
        arena, entry, "name",
        vkr_bakery_json_cstr(arena, scene->name ? scene->name : ""));
    vkr_bakery_json_set(arena, entry, "scene",
                        vkr_bakery_json_cstr(arena, scene->identity));
    vkr_bakery_json_set(arena, entry, "overlay",
                        vkr_bakery_json_cstr(arena, scene->overlay));
    vkr_bakery_json_append(scenes, entry);
  }
  vkr_bakery_json_set(arena, game, "scenes", scenes);
  VkrBakeryJson *fonts = vkr_bakery_json_array(arena);
  for (const VkrBakeryJson *font = package->fonts->first; font;
       font = font->next) {
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, entry, "name",
                        vkr_bakery_json_string(arena, font->key));
    vkr_bakery_json_set(arena, entry, "config",
                        vkr_bakery_json_clone(arena, font));
    vkr_bakery_json_append(fonts, entry);
  }
  vkr_bakery_json_set(arena, game, "fonts", fonts);
  vkr_bakery_json_set(
      arena, game, "window",
      vkr_bakery_json_clone(arena, vkr_bakery_json_get(settings, "window")));
  const VkrBakeryJson *graphics = vkr_bakery_json_get(settings, "graphics");
  vkr_bakery_json_set(arena, game, "graphics",
                      graphics ? vkr_bakery_json_clone(arena, graphics)
                               : vkr_bakery_json_object(arena));
  return game;
}

vkr_internal bool8_t vkr_package_stage_runtime(VkrPackage *package,
                                               const VkrBundleItem *items,
                                               uint32_t item_count) {
  Arena *arena = package->arena;
  const char *executable = vkr_package_printf(
      package, "%s" VKR_PACKAGE_EXECUTABLE_SUFFIX, package->executable);
  char destination[VKR_BAKERY_PATH_CAPACITY];
  char shaders[VKR_BAKERY_PATH_CAPACITY];
  char catalog[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bakery_path_join(destination, sizeof(destination), package->staging,
                            executable) ||
      !vkr_bakery_clone_or_copy(package->player, destination)) {
    vkr_bakery_event_diag(
        package->stage_id, VKR_BAKERY_DIAG_BUNDLE_MISSING_RUNTIME,
        package->player, 0u, 0u, "cannot copy the player", NULL);
    return false_v;
  }
#if !defined(_WIN32)
  /* A byte copy, unlike a clone, drops the template's mode. */
  (void)chmod(destination, 0755);
#endif
  if (!vkr_bakery_path_join(catalog, sizeof(catalog), package->shaders,
                            VKR_PACKAGE_BACKEND) ||
      !vkr_bakery_path_join(shaders, sizeof(shaders), package->staging,
                            "shaders/" VKR_PACKAGE_BACKEND) ||
      !vkr_bundle_copy_tree(catalog, shaders)) {
    vkr_bakery_event_diag(package->stage_id,
                          VKR_BAKERY_DIAG_BUNDLE_MISSING_RUNTIME, catalog, 0u,
                          0u, "cannot copy the shader catalog", NULL);
    return false_v;
  }

  VkrBakeryJson *description = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, description, "version",
                      vkr_bakery_json_int(arena, 2));
  vkr_bakery_json_set(arena, description, "name",
                      vkr_bakery_json_cstr(arena, package->project_name));
  vkr_bakery_json_set(arena, description, "profile",
                      vkr_bakery_json_cstr(arena, package->profile_name));
  vkr_bakery_json_set(arena, description, "platform",
                      vkr_bakery_json_cstr(arena, VKR_PACKAGE_PLATFORM));
  vkr_bakery_json_set(arena, description, "backend",
                      vkr_bakery_json_cstr(arena, VKR_PACKAGE_BACKEND));
  vkr_bakery_json_set(arena, description, "config",
                      vkr_bakery_json_cstr(arena, package->config));
  vkr_bakery_json_set(arena, description, "executable",
                      vkr_bakery_json_cstr(arena, executable));
  VkrBakeryJson *packs = vkr_bakery_json_array(arena);
  vkr_bakery_json_append(packs,
                         vkr_bakery_json_cstr(arena, "content/game.vkpak"));
  vkr_bakery_json_append(packs,
                         vkr_bakery_json_cstr(arena, "content/engine.vkpak"));
  vkr_bakery_json_set(arena, description, "packs", packs);
  VkrBakeryJson *products = vkr_bakery_json_object(arena);
  for (uint32_t i = 0u; i < item_count; ++i) {
    char hex[VKR_BAKERY_SHA256_HEX];
    for (uint32_t b = 0u; b < 32u; ++b) {
      snprintf(hex + b * 2u, 3u, "%02x", items[i].sha256[b]);
    }
    vkr_bakery_json_set(arena, products, items[i].identity,
                        vkr_bakery_json_cstr(arena, hex));
  }
  vkr_bakery_json_set(arena, description, "products", products);
  vkr_bakery_json_set(arena, description, "scripts",
                      vkr_bakery_json_array(arena));
  vkr_bakery_json_set(arena, description, "game",
                      vkr_package_game_object(package));
  char path[VKR_BAKERY_PATH_CAPACITY];
  (void)vkr_bakery_path_join(path, sizeof(path), package->staging,
                             "bundle.json");
  return vkr_package_write_json(package, path, description);
}

vkr_internal bool8_t vkr_package_verify(VkrPackage *package) {
  static const char *const archives[] = {"content/game.vkpak",
                                         "content/engine.vkpak"};
  for (uint32_t i = 0u; i < ArrayCount(archives); ++i) {
    char path[VKR_BAKERY_PATH_CAPACITY];
    if (!vkr_bakery_path_join(path, sizeof(path), package->staging,
                              archives[i]) ||
        !vkr_bundle_verify(path)) {
      return vkr_package_fail(package, path, "the archive does not verify");
    }
  }
  return true_v;
}

/* Replaces `<out>` with the verified staging directory. A previous package
   moves aside first and returns when the rename fails. */
vkr_internal bool8_t vkr_package_publish(VkrPackage *package) {
  if (!vkr_bakery_remove_tree(package->work)) {
    return vkr_package_fail(package, package->work,
                            "cannot remove the work folder");
  }
  char previous[VKR_BAKERY_PATH_CAPACITY];
  (void)snprintf(previous, sizeof(previous), "%s.previous", package->out);
  const bool8_t replacing = vkr_bakery_is_directory(package->out);
  if (!vkr_bakery_remove_tree(previous) ||
      (replacing && !vkr_bakery_rename(package->out, previous, false_v))) {
    return vkr_package_fail(package, package->out,
                            "cannot move the previous package aside");
  }
  if (!vkr_bakery_rename(package->staging, package->out, false_v)) {
    if (replacing) {
      (void)vkr_bakery_rename(previous, package->out, false_v);
    }
    return vkr_package_fail(package, package->out,
                            "cannot publish the package");
  }
  (void)vkr_bakery_remove_tree(previous);
  return true_v;
}

vkr_internal void vkr_package_write_report(VkrPackage *package, bool8_t ok) {
  Arena *arena = package->arena;
  char stamp[32];
  vkr_bakery_utc_timestamp(stamp);
  char name[64];
  uint32_t length = 0u;
  for (const char *c = stamp; *c && length + 1u < sizeof(name); ++c) {
    name[length++] = *c == ':' ? '-' : *c;
  }
  name[length] = 0;
  (void)snprintf(package->report_path, sizeof(package->report_path),
                 "%s/logs/builds/%s-%u.json", package->workspace, name,
                 (uint32_t)(vkr_bakery_monotonic_seconds() * 1000.0) % 1000u);
  VkrBakeryJson *report = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, report, "version", vkr_bakery_json_int(arena, 1));
  vkr_bakery_json_set(arena, report, "status",
                      vkr_bakery_json_cstr(arena, ok ? "complete"
                                                  : vkr_package_cancelled()
                                                      ? "cancelled"
                                                      : "failed"));
  vkr_bakery_json_set(arena, report, "created",
                      vkr_bakery_json_cstr(arena, stamp));
  vkr_bakery_json_set(arena, report, "project",
                      vkr_bakery_json_cstr(arena, package->project_path));
  vkr_bakery_json_set(arena, report, "profile",
                      vkr_bakery_json_cstr(arena, package->profile_name
                                                      ? package->profile_name
                                                      : ""));
  vkr_bakery_json_set(
      arena, report, "config",
      vkr_bakery_json_cstr(arena, package->config ? package->config : ""));
  vkr_bakery_json_set(arena, report, "platform",
                      vkr_bakery_json_cstr(arena, VKR_PACKAGE_PLATFORM));
  vkr_bakery_json_set(arena, report, "output",
                      vkr_bakery_json_cstr(arena, package->out));
  vkr_bakery_json_set(
      arena, report, "executable",
      vkr_bakery_json_cstr(
          arena,
          package->executable
              ? vkr_package_printf(package, "%s" VKR_PACKAGE_EXECUTABLE_SUFFIX,
                                   package->executable)
              : ""));
  vkr_bakery_json_set(arena, report, "stages", package->stages);
  vkr_bakery_json_set(arena, report, "warnings", package->warnings);
  vkr_bakery_json_set(arena, report, "package",
                      package->summary ? package->summary
                                       : vkr_bakery_json_object(arena));
  if (package->project_assets) {
    vkr_bakery_json_set(arena, report, "project_assets",
                        package->project_assets);
  }
  if (vkr_package_write_json(package, package->report_path, report)) {
    vkr_bakery_event_log(0u, "info", package->report_path,
                         strlen(package->report_path));
    vkr_bakery_print("report %s\n", package->report_path);
  }
}

// =============================================================================
// Command
// =============================================================================

vkr_internal bool8_t vkr_package_open(VkrPackage *package,
                                      const char *project) {
  if (!vkr_project_resolve(project, true_v, package->project_root,
                           sizeof(package->project_root))) {
    return vkr_package_fail(package, project, "no project at %s", project);
  }
  (void)vkr_bakery_path_join(package->project_path,
                             sizeof(package->project_path),
                             package->project_root, "project.json");
  char projects[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_path_parent(projects, sizeof(projects), package->project_root);
  vkr_bakery_path_parent(package->workspace, sizeof(package->workspace),
                         projects);
  vkr_bakery_path_parent(package->workspace_directory,
                         sizeof(package->workspace_directory),
                         package->workspace);
  (void)vkr_bakery_path_join(package->editor_bundle,
                             sizeof(package->editor_bundle), package->workspace,
                             "editor/bundles/1");
  char workspace_manifest[VKR_BAKERY_PATH_CAPACITY];
  (void)vkr_bakery_path_join(workspace_manifest, sizeof(workspace_manifest),
                             package->workspace, "workspace.json");
  if (strcmp(vkr_bakery_path_name(projects), "projects") != 0 ||
      !vkr_bakery_is_file(workspace_manifest)) {
    return vkr_package_fail(package, project,
                            "a project must be in the projects directory of "
                            "a workspace");
  }
  package->project =
      vkr_package_read_json(package, package->project_path, true_v);
  package->project_name = vkr_package_text(package, package->project, "name");
  if (!package->project || !package->project_name) {
    return vkr_package_fail(package, package->project_path,
                            "project.json has no name");
  }
  char game_path[VKR_BAKERY_PATH_CAPACITY];
  (void)vkr_bakery_path_join(game_path, sizeof(game_path),
                             package->project_root, "game.json");
  package->game = vkr_bakery_is_file(game_path)
                      ? vkr_package_read_json(package, game_path, true_v)
                      : vkr_package_default_game(package);
  return package->game && vkr_package_game_settings(package, game_path) &&
         vkr_package_select_profile(package, game_path) &&
         vkr_package_check_template(package) &&
         vkr_package_check_output(package);
}

int vkr_bakery_bundle_project(VkrBakeryCli *cli, const char *project) {
  vkr_bakery_install_cancel_signals();
  Arena *arena = arena_create(GB(1), MB(4));
  VkrPackage *package =
      arena ? (VkrPackage *)arena_alloc(arena, sizeof(VkrPackage),
                                        ARENA_MEMORY_TAG_STRUCT)
            : NULL;
  if (!package) {
    if (arena) {
      arena_destroy(arena);
    }
    return VKR_BAKERY_EXIT_ENVIRONMENT;
  }
  MemZero(package, sizeof(*package));
  package->cli = cli;
  package->arena = arena;
  package->fonts = vkr_bakery_json_object(arena);
  package->warnings = vkr_bakery_json_array(arena);
  package->stages = vkr_bakery_json_array(arena);
  VkrBundleItem *items = NULL;
  uint32_t item_count = 0u;
  bool8_t changed = false_v;

  vkr_package_stage_begin(package, "Validate");
  bool8_t ok = vkr_package_open(package, project) &&
               vkr_package_lower_world(package) &&
               vkr_package_lower_scenes(package);
  /* A project without a default font lowers scene text to the font the
     editor registers at startup; the package names the same engine file. */
  if (ok && !vkr_bakery_json_get(package->fonts, "default-scene-font")) {
    vkr_bakery_json_set(
        arena, package->fonts, "default-scene-font",
        vkr_bakery_json_cstr(arena, "assets/fonts/UbuntuMono-cooked.fontcfg"));
  }
  if (ok && package->startup < package->scene_count &&
      !package->scenes[package->startup].has_player) {
    vkr_package_warn(package,
                     "the startup scene has no player; the game starts with "
                     "a free camera");
  }
  const bool8_t opened = package->workspace[0] && package->out[0];
  vkr_package_stage_end(package, ok, NULL);

  if (ok) {
    vkr_package_stage_begin(package, "Finalize");
    ok = vkr_package_finalize(package, &changed);
    vkr_package_stage_end(package, ok, changed ? NULL : "nothing to finalize");
  }
  if (ok) {
    vkr_package_stage_begin(package, "Bake");
    ok = !package->bake_lighting || vkr_package_bake(package);
    changed = changed || package->bake_lighting;
    vkr_package_stage_end(package, ok,
                          package->bake_lighting ? NULL : "not requested");
  }
  if (ok) {
    /* Validation lowered every scene; a stage that published since lowers
       them again against the final content. */
    vkr_package_stage_begin(package, "Lower");
    ok = !changed || vkr_package_lower_scenes(package);
    vkr_package_stage_end(package, ok,
                          changed ? NULL : "lowered during validation");
  }
  if (ok) {
    vkr_package_stage_begin(package, "Pack");
    ok = vkr_package_pack(package, &items, &item_count);
    vkr_package_stage_end(package, ok, NULL);
  }
  if (ok && !cli->config.dry_run) {
    vkr_package_stage_begin(package, "Stage runtime");
    ok = vkr_package_stage_runtime(package, items, item_count);
    vkr_package_stage_end(package, ok, NULL);
  }
  if (ok && !cli->config.dry_run) {
    vkr_package_stage_begin(package, "Verify and report");
    ok = vkr_package_verify(package) && vkr_package_publish(package);
    vkr_package_stage_end(package, ok, NULL);
  }
  if (ok && !cli->config.dry_run) {
    vkr_bakery_print("packaged %s in %s\n", package->project_name,
                     package->out);
  }
  /* A failed, cancelled or dry run leaves no staging behind when it can. */
  if ((!ok || cli->config.dry_run) && package->staging[0]) {
    (void)vkr_bakery_remove_tree(package->staging);
  }
  if (opened) {
    vkr_package_write_report(package, ok);
  }
  const int code = ok                        ? VKR_BAKERY_EXIT_OK
                   : vkr_package_cancelled() ? VKR_BAKERY_EXIT_CANCELLED
                                             : VKR_BAKERY_EXIT_FAILED;
  free(items);
  arena_destroy(arena);
  return code;
}
