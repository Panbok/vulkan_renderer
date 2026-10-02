#include "editor_scripts.h"

#include "editor_install.h"

#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_json.h"
#include "core/vkr_threads.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_platform.h"
#include "renderer/systems/vkr_scene_types.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#if defined(PLATFORM_WINDOWS)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#define SCRIPTS_NONE UINT32_MAX
/* The runtime's name for the open project's library; one project is open at
   a time and closing it retires the library. */
#define SCRIPTS_PROJECT_LIBRARY "project"
#if defined(PLATFORM_WINDOWS)
#define SCRIPTS_LIBRARY_PREFIX ""
#define SCRIPTS_LIBRARY_SUFFIX ".dll"
#else
#define SCRIPTS_LIBRARY_PREFIX "lib"
#define SCRIPTS_LIBRARY_SUFFIX ".dylib"
#endif

typedef struct ScriptsBuild {
  const char *arguments[10];
  uint32_t argument_count;
  char events[VKR_EDITOR_SCRIPT_PATH];
  char log[VKR_EDITOR_SCRIPT_PATH];
} ScriptsBuild;

struct VkrEditorScripts {
  VkrAllocator *allocator;
  bool8_t open;
  char scripts_directory[VKR_EDITOR_SCRIPT_PATH];
  char output_root[VKR_EDITOR_SCRIPT_PATH];
  VkrEditorScriptModule modules[VKR_EDITOR_SCRIPT_MODULE_MAX];
  uint32_t module_count;
  VkrEditorScriptFile files[VKR_EDITOR_SCRIPT_FILE_MAX];
  uint32_t file_count;
  VkrEditorScriptDiagnostic diagnostics[VKR_EDITOR_SCRIPT_DIAGNOSTIC_MAX];
  uint32_t diagnostic_count;
  uint64_t revision;
  /* The Scripts folder itself, for modules added or removed on disk. */
  uint32_t folder_watch;
  EditorBakeryService *service;
  /* The project library every package builds into. */
  char library[VKR_EDITOR_SCRIPT_PATH];
  VkrEditorScriptStatus status;
  char message[256];
  /* Bytes of the library the runtime last loaded, to skip identical builds. */
  uint64_t loaded_fingerprint;
  uint64_t pending_fingerprint;
  /* A build was asked for while another one ran or none could start. */
  bool8_t rebuild;
  /* The project opened without a library: its first build runs on the
     worker, and documents wait for it (vkr_editor_scripts_settling). */
  bool8_t first_build;
  /* The worker's build: process result and completion. */
  VkrThread worker;
  bool8_t worker_live;
  VkrAtomicBool worker_done;
  int32_t exit_code;
  bool8_t process_ok;
  uint64_t result_serial;
  /* Successful loads applied; see vkr_editor_scripts_load_serial. */
  uint64_t load_serial;
  ScriptsBuild worker_build;
};

// =============================================================================
// Files
// =============================================================================

static bool8_t scripts_join(char *out, const char *directory,
                            const char *name) {
  const int32_t written =
      snprintf(out, VKR_EDITOR_SCRIPT_PATH, "%s/%s", directory, name);
  return written > 0 && written < (int32_t)VKR_EDITOR_SCRIPT_PATH;
}

static bool8_t scripts_is_file(const char *path) {
  FILE *file = file_fopen(path, "rb");
  if (file) {
    fclose(file);
  }
  return file != NULL;
}

/* Whole file into allocator memory with a trailing NUL, or NULL. */
static uint8_t *scripts_read(VkrAllocator *allocator, const char *path,
                             uint64_t *out_length) {
  FILE *file = file_fopen(path, "rb");
  if (!file) {
    return NULL;
  }
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (fseek(file, 0, SEEK_END) == 0) {
    const long size = ftell(file);
    if (size >= 0 && size < (long)(64u << 20) &&
        fseek(file, 0, SEEK_SET) == 0) {
      data = vkr_allocator_alloc(allocator, (uint64_t)size + 1u,
                                 VKR_ALLOCATOR_MEMORY_TAG_FILE);
      if (data && fread(data, 1u, (size_t)size, file) == (size_t)size) {
        data[size] = 0;
        length = (uint64_t)size;
      } else if (data) {
        vkr_allocator_free(allocator, data, (uint64_t)size + 1u,
                           VKR_ALLOCATOR_MEMORY_TAG_FILE);
        data = NULL;
      }
    }
  }
  fclose(file);
  *out_length = length;
  return data;
}

static bool8_t scripts_write(const char *path, const char *text) {
  FILE *file = file_fopen(path, "wb");
  if (!file) {
    return false_v;
  }
  const size_t length = strlen(text);
  const bool8_t written = fwrite(text, 1u, length, file) == length;
  return fclose(file) == 0 && written;
}

/* The directory helper borrows a scoped arena for its path copies. */
static bool8_t scripts_make_directory(VkrAllocator *allocator,
                                      const char *path) {
  (void)allocator;
  Arena *arena = arena_create(KB(8), KB(8));
  VkrAllocator scratch = {.ctx = arena};
  if (!arena || !vkr_allocator_arena(&scratch)) {
    if (arena) {
      arena_destroy(arena);
    }
    return false_v;
  }
  const String8 directory =
      string8_create_from_cstr((const uint8_t *)path, strlen(path));
  const bool8_t made = file_ensure_directory(&scratch, &directory);
  vkr_allocator_release_global_accounting(&scratch);
  arena_destroy(arena);
  return made;
}

/* FNV-1a over a file's bytes; zero when it cannot be read. */
static uint64_t scripts_fingerprint(VkrAllocator *allocator, const char *path) {
  uint64_t length = 0u;
  uint8_t *data = scripts_read(allocator, path, &length);
  if (!data) {
    return 0u;
  }
  uint64_t hash = UINT64_C(1469598103934665603);
  for (uint64_t i = 0; i < length; ++i) {
    hash = (hash ^ data[i]) * UINT64_C(1099511628211);
  }
  vkr_allocator_free(allocator, data, length + 1u,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  return hash ? hash : 1u;
}

typedef void (*ScriptsEntryVisit)(void *context, const char *name,
                                  bool8_t directory);

/* Each entry of `path` except dot entries, in the platform's order. */
static void scripts_list(const char *path, ScriptsEntryVisit visit,
                         void *context) {
#if defined(PLATFORM_WINDOWS)
  char pattern[VKR_EDITOR_SCRIPT_PATH];
  wchar_t wide[VKR_EDITOR_SCRIPT_PATH];
  if (!scripts_join(pattern, path, "*") ||
      !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pattern, -1, wide,
                           ArrayCount(wide))) {
    return;
  }
  WIN32_FIND_DATAW entry;
  HANDLE search = FindFirstFileW(wide, &entry);
  if (search == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    char name[256];
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry.cFileName, -1,
                            name, sizeof(name), NULL, NULL) &&
        name[0] != '.') {
      visit(context, name,
            (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    }
  } while (FindNextFileW(search, &entry));
  FindClose(search);
#else
  DIR *directory = opendir(path);
  if (!directory) {
    return;
  }
  struct dirent *entry;
  while ((entry = readdir(directory))) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    char child[VKR_EDITOR_SCRIPT_PATH];
    struct stat info;
    const bool8_t is_directory = scripts_join(child, path, entry->d_name) &&
                                 stat(child, &info) == 0 &&
                                 S_ISDIR(info.st_mode);
    visit(context, entry->d_name, is_directory);
  }
  closedir(directory);
#endif
}

static bool8_t scripts_name_valid(const char *name) {
  const size_t length = strlen(name);
  if (!length || length >= VKR_EDITOR_SCRIPT_NAME ||
      !(isalpha((unsigned char)name[0]) || name[0] == '_')) {
    return false_v;
  }
  for (size_t i = 0; i < length; ++i) {
    if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) {
      return false_v;
    }
  }
  return true_v;
}

// =============================================================================
// Discovery
// =============================================================================

typedef struct ScriptsScan {
  VkrEditorScripts *scripts;
  uint32_t module;
} ScriptsScan;

static bool8_t scripts_source_name(const char *name) {
  const char *dot = strrchr(name, '.');
  return dot &&
         (!strcmp(dot, ".c") || !strcmp(dot, ".h") || !strcmp(dot, ".json"));
}

static void scripts_visit_file(void *context, const char *name,
                               bool8_t directory) {
  ScriptsScan *scan = context;
  VkrEditorScripts *scripts = scan->scripts;
  if (directory || !scripts_source_name(name) ||
      scripts->file_count == VKR_EDITOR_SCRIPT_FILE_MAX) {
    return;
  }
  VkrEditorScriptFile *file = &scripts->files[scripts->file_count];
  if (!scripts_join(file->path, scripts->modules[scan->module].directory,
                    name)) {
    return;
  }
  snprintf(file->name, sizeof(file->name), "%s", name);
  file->module = scan->module;
  scripts->file_count++;
}

/* The package's kind and dependencies from its description. Strings are
 * read by hand: a dependency list is a flat array of names. */
static void scripts_read_description(VkrEditorScripts *scripts,
                                     VkrEditorScriptModule *module) {
  uint64_t length = 0u;
  uint8_t *data =
      scripts_read(scripts->allocator, module->description, &length);
  if (!data) {
    return;
  }
  const char *text = (const char *)data;
  const char *kind = strstr(text, "\"kind\"");
  if (kind) {
    const char *value = strchr(kind + 6, '"');
    module->library_kind = value && !strncmp(value, "\"library\"", 9);
  }
  const char *dependencies = strstr(text, "\"dependencies\"");
  const char *open = dependencies ? strchr(dependencies, '[') : NULL;
  const char *close = open ? strchr(open, ']') : NULL;
  for (const char *at = open;
       at && at < close &&
       module->dependency_count < VKR_EDITOR_SCRIPT_DEPENDENCY_MAX;) {
    const char *start = strchr(at, '"');
    const char *end = start && start < close ? strchr(start + 1, '"') : NULL;
    if (!end || end > close) {
      break;
    }
    const uint64_t size = (uint64_t)(end - start - 1);
    if (size && size < VKR_EDITOR_SCRIPT_NAME) {
      char *name = module->dependencies[module->dependency_count++];
      MemCopy(name, start + 1, size);
      name[size] = '\0';
    }
    at = end + 1;
  }
  vkr_allocator_free(scripts->allocator, data, length + 1u,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
}

/* A package is a folder holding `<folder>.script.json`. */
static void scripts_visit_module(void *context, const char *name,
                                 bool8_t directory) {
  ScriptsScan *scan = context;
  VkrEditorScripts *scripts = scan->scripts;
  if (!directory || !scripts_name_valid(name) ||
      scripts->module_count == VKR_EDITOR_SCRIPT_MODULE_MAX) {
    return;
  }
  VkrEditorScriptModule module = {.watch = 0u};
  snprintf(module.name, sizeof(module.name), "%s", name);
  char description[VKR_EDITOR_SCRIPT_PATH];
  snprintf(description, sizeof(description), "%s.script.json", name);
  if (!scripts_join(module.directory, scripts->scripts_directory, name) ||
      !scripts_join(module.description, module.directory, description) ||
      !scripts_is_file(module.description)) {
    return;
  }
  scripts_read_description(scripts, &module);
  scan->module = scripts->module_count;
  scripts->modules[scripts->module_count++] = module;
  scripts_list(module.directory, scripts_visit_file, scan);
}

/* Shows the project's state on every package. */
static void scripts_set_status(VkrEditorScripts *scripts,
                               VkrEditorScriptStatus status,
                               const char *message) {
  scripts->status = status;
  snprintf(scripts->message, sizeof(scripts->message), "%s", message);
  for (uint32_t i = 0; i < scripts->module_count; ++i) {
    scripts->modules[i].status = status;
    snprintf(scripts->modules[i].message, sizeof(scripts->modules[i].message),
             "%s", message);
  }
}

/* Rebuilds the package and file lists, keeping the watches of packages that
 * remain. */
static void scripts_scan(VkrEditorScripts *scripts,
                         EditorBakeryService *service) {
  VkrEditorScriptModule previous[VKR_EDITOR_SCRIPT_MODULE_MAX];
  const uint32_t previous_count = scripts->module_count;
  MemCopy(previous, scripts->modules, sizeof(previous));
  scripts->module_count = 0u;
  scripts->file_count = 0u;
  ScriptsScan scan = {.scripts = scripts};
  scripts_list(scripts->scripts_directory, scripts_visit_module, &scan);
  for (uint32_t i = 0; i < scripts->module_count; ++i) {
    VkrEditorScriptModule *module = &scripts->modules[i];
    module->status = scripts->status;
    snprintf(module->message, sizeof(module->message), "%s", scripts->message);
    for (uint32_t p = 0; p < previous_count; ++p) {
      if (!strcmp(previous[p].name, module->name)) {
        module->watch = previous[p].watch;
        previous[p].watch = 0u;
      }
    }
    if (!module->watch && service) {
      const char *paths[] = {module->directory};
      module->watch = editor_bakery_service_watch(service, paths, 1u, NULL, 0u);
    }
  }
  for (uint32_t p = 0; p < previous_count; ++p) {
    if (previous[p].watch && service) {
      editor_bakery_service_unwatch(service, previous[p].watch);
    }
  }
  scripts->revision++;
}

static uint32_t scripts_index_of(const VkrEditorScripts *scripts,
                                 const char *path) {
  for (uint32_t i = 0; path && i < scripts->module_count; ++i) {
    const size_t length = strlen(scripts->modules[i].directory);
    if (!strncmp(path, scripts->modules[i].directory, length) &&
        (path[length] == '/' || path[length] == '\\' || !path[length])) {
      return i;
    }
  }
  return SCRIPTS_NONE;
}

// =============================================================================
// Builds
// =============================================================================

/* One build covers the whole Scripts folder: every package links into the
 * project library (`vkr_bakery scripts`). */
static bool8_t scripts_build_prepare(VkrEditorScripts *scripts,
                                     ScriptsBuild *build) {
  *build = (ScriptsBuild){0};
  if (!scripts_make_directory(scripts->allocator, scripts->output_root) ||
      !scripts_join(build->events, scripts->output_root, "build.events") ||
      !scripts_join(build->log, scripts->output_root, "build.log")) {
    return false_v;
  }
  const char *arguments[] = {
      "scripts", scripts->scripts_directory, "--name", SCRIPTS_PROJECT_LIBRARY,
      "--root",  scripts->scripts_directory, "--out",  scripts->output_root,
      "--json"};
  MemCopy(build->arguments, arguments, sizeof(arguments));
  build->argument_count = ArrayCount(arguments);
  return true_v;
}

static bool8_t scripts_build_run(const ScriptsBuild *build,
                                 int32_t *exit_code) {
  const VkrPlatformProcessConfig config = {
      .executable = vkr_editor_tool_path(VKR_EDITOR_TOOL_BAKERY),
      .arguments = build->arguments,
      .argument_count = build->argument_count,
      .stdout_path = build->events,
      .stderr_path = build->log,
      .timeout_ms = 120000u,
      .termination_grace_ms = 500u,
      .hidden = true_v,
      .terminate_process_tree = true_v,
  };
  bool8_t timed_out = false_v;
  return vkr_platform_process_run(&config, exit_code, &timed_out) && !timed_out;
}

/* Runs the prepared build; the project's paths stay unchanged meanwhile. */
static void *scripts_worker(void *context) {
  VkrEditorScripts *scripts = context;
  scripts->process_ok =
      scripts_build_run(&scripts->worker_build, &scripts->exit_code);
  vkr_atomic_bool_store(&scripts->worker_done, true_v,
                        VKR_MEMORY_ORDER_RELEASE);
  return NULL;
}

static void scripts_join_worker(VkrEditorScripts *scripts) {
  if (scripts->worker_live) {
    (void)vkr_thread_join(scripts->worker);
    (void)vkr_thread_destroy(scripts->allocator, &scripts->worker);
    scripts->worker_live = false_v;
  }
}

/* Replaces the diagnostics with those the project build reported, each
 * filed under the package holding its file. */
static void scripts_read_diagnostics(VkrEditorScripts *scripts,
                                     const char *events_path) {
  scripts->diagnostic_count = 0u;
  uint64_t length = 0u;
  uint8_t *events = scripts_read(scripts->allocator, events_path, &length);
  if (!events) {
    return;
  }
  Arena *arena = arena_create(KB(64), KB(64));
  VkrAllocator scratch = {.ctx = arena};
  if (arena && vkr_allocator_arena(&scratch)) {
    char *line = (char *)events;
    while (line && *line &&
           scripts->diagnostic_count < VKR_EDITOR_SCRIPT_DIAGNOSTIC_MAX) {
      char *end = strchr(line, '\n');
      if (end) {
        *end = '\0';
      }
      if (line[0] == '{' && strstr(line, "\"ev\":\"diag\"")) {
        const VkrJsonReader root = vkr_json_reader_from_string(
            string8_create_from_cstr((const uint8_t *)line, strlen(line)));
        VkrJsonReader field = root;
        String8 source = {0};
        String8 message = {0};
        String8 severity = {0};
        int32_t line_number = 0;
        int32_t column = 0;
        field = root;
        const bool8_t has_source =
            vkr_json_find_root_field(&field, "source") &&
            vkr_json_parse_string_decoded(&field, &scratch, &source);
        field = root;
        (void)(vkr_json_find_root_field(&field, "message") &&
               vkr_json_parse_string_decoded(&field, &scratch, &message));
        field = root;
        (void)(vkr_json_find_root_field(&field, "severity") &&
               vkr_json_parse_string(&field, &severity));
        field = root;
        (void)(vkr_json_find_root_field(&field, "line") &&
               vkr_json_parse_int(&field, &line_number));
        field = root;
        (void)(vkr_json_find_root_field(&field, "column") &&
               vkr_json_parse_int(&field, &column));
        VkrEditorScriptDiagnostic *diagnostic =
            &scripts->diagnostics[scripts->diagnostic_count];
        *diagnostic = (VkrEditorScriptDiagnostic){
            .module = SCRIPTS_NONE,
            .line = line_number > 0 ? (uint32_t)line_number : 0u,
            .column = column > 0 ? (uint32_t)column : 0u,
            .error = severity.length == 5u &&
                     MemCompare(severity.str, "error", 5u) == 0};
        /* Bakery names files under its --root, the Scripts folder; compiler
           messages name them absolutely. */
        if (has_source && source.length &&
            (source.str[0] == '/' ||
             (source.length > 1u && source.str[1] == ':'))) {
          snprintf(diagnostic->path, sizeof(diagnostic->path), "%.*s",
                   (int32_t)source.length, (const char *)source.str);
        } else if (has_source) {
          snprintf(diagnostic->path, sizeof(diagnostic->path), "%s/%.*s",
                   scripts->scripts_directory, (int32_t)source.length,
                   (const char *)source.str);
        }
        diagnostic->module = scripts_index_of(scripts, diagnostic->path);
        snprintf(diagnostic->message, sizeof(diagnostic->message), "%.*s",
                 (int32_t)message.length, (const char *)message.str);
        scripts->diagnostic_count++;
      }
      line = end ? end + 1 : NULL;
    }
    vkr_allocator_release_global_accounting(&scratch);
  }
  if (arena) {
    arena_destroy(arena);
  }
  vkr_allocator_free(scripts->allocator, events, length + 1u,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
}

/* After a finished build: diagnostics, then a load request when the
 * library changed. */
static void scripts_build_finished(VkrEditorScripts *scripts,
                                   bool8_t process_ok, int32_t exit_code,
                                   const char *events_path,
                                   const VkrSampleUiFrame *frame) {
  scripts_read_diagnostics(scripts, events_path);
  if (!process_ok || exit_code != 0) {
    uint32_t errors = 0u;
    for (uint32_t i = 0; i < scripts->diagnostic_count; ++i) {
      errors += scripts->diagnostics[i].error;
    }
    char message[256];
    snprintf(message, sizeof(message),
             process_ok ? "Build failed with %u error%s; the previous code "
                          "keeps running"
                        : "Bakery could not run the build",
             errors, errors == 1u ? "" : "s");
    scripts_set_status(scripts, VKR_EDITOR_SCRIPT_BUILD_FAILED, message);
    log_error("Scripts: %s", message);
    return;
  }
  const uint64_t fingerprint =
      scripts_fingerprint(scripts->allocator, scripts->library);
  if (fingerprint && fingerprint == scripts->loaded_fingerprint) {
    scripts_set_status(scripts, VKR_EDITOR_SCRIPT_LOADED, "Up to date");
    return;
  }
  VkrSampleScriptRequest *request = frame->script_request;
  if (!request || request->load_count == VKR_SAMPLE_SCRIPT_LOAD_MAX) {
    scripts->rebuild = true_v;
    return;
  }
  VkrSampleScriptLoad *load = &request->loads[request->load_count++];
  *load = (VkrSampleScriptLoad){.project = true_v};
  snprintf(load->name, sizeof(load->name), "%s", SCRIPTS_PROJECT_LIBRARY);
  snprintf(load->path, sizeof(load->path), "%s", scripts->library);
  scripts->pending_fingerprint = fingerprint;
  scripts_set_status(scripts, VKR_EDITOR_SCRIPT_LOADING, "Loading");
}

static bool8_t scripts_start_build(VkrEditorScripts *scripts) {
  if (scripts->worker_live) {
    scripts->rebuild = true_v;
    return false_v;
  }
  if (!scripts_build_prepare(scripts, &scripts->worker_build)) {
    return false_v;
  }
  vkr_atomic_bool_store(&scripts->worker_done, false_v,
                        VKR_MEMORY_ORDER_RELAXED);
  if (!vkr_thread_create(scripts->allocator, &scripts->worker, scripts_worker,
                         scripts)) {
    return false_v;
  }
  scripts->worker_live = true_v;
  scripts->rebuild = false_v;
  scripts_set_status(scripts, VKR_EDITOR_SCRIPT_BUILDING, "Building");
  return true_v;
}

// =============================================================================
// Lifetime
// =============================================================================

VkrEditorScripts *vkr_editor_scripts_create(VkrAllocator *allocator) {
  VkrEditorScripts *scripts = vkr_allocator_alloc(
      allocator, sizeof(*scripts), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (scripts) {
    MemZero(scripts, sizeof(*scripts));
    scripts->allocator = allocator;
  }
  return scripts;
}

void vkr_editor_scripts_destroy(VkrEditorScripts *scripts) {
  if (!scripts) {
    return;
  }
  scripts_join_worker(scripts);
  vkr_allocator_free(scripts->allocator, scripts, sizeof(*scripts),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

void vkr_editor_scripts_open(VkrEditorScripts *scripts,
                             const char *scripts_directory,
                             const char *output_root,
                             EditorBakeryService *service,
                             const VkrSampleUiFrame *frame) {
  vkr_editor_scripts_close_project(scripts, service, frame);
  snprintf(scripts->scripts_directory, sizeof(scripts->scripts_directory), "%s",
           scripts_directory);
  snprintf(scripts->output_root, sizeof(scripts->output_root), "%s",
           output_root);
  char library[VKR_EDITOR_SCRIPT_NAME + 16u];
  snprintf(library, sizeof(library), "%s%s%s", SCRIPTS_LIBRARY_PREFIX,
           SCRIPTS_PROJECT_LIBRARY, SCRIPTS_LIBRARY_SUFFIX);
  if (!scripts_join(scripts->library, scripts->output_root, library)) {
    scripts->library[0] = '\0';
  }
  scripts->open = true_v;
  scripts->service = service;
  scripts->loaded_fingerprint = 0u;
  scripts->status = VKR_EDITOR_SCRIPT_UNBUILT;
  scripts->message[0] = '\0';
  scripts_scan(scripts, service);
  if (service) {
    const char *paths[] = {scripts->scripts_directory};
    scripts->folder_watch =
        editor_bakery_service_watch(service, paths, 1u, NULL, 0u);
  }
  if (!scripts->module_count) {
    return;
  }
  if (scripts_is_file(scripts->library)) {
    /* Load the last build now; a background build replaces it when the
       sources moved on. */
    scripts_build_finished(scripts, true_v, 0, "", frame);
    scripts->rebuild = true_v;
    return;
  }
  /* No library yet: build on the worker while frames continue. */
  scripts->first_build = scripts_start_build(scripts);
  if (!scripts->first_build) {
    scripts_set_status(scripts, VKR_EDITOR_SCRIPT_BUILD_FAILED,
                       "Bakery could not start the build");
  }
}

bool8_t vkr_editor_scripts_settling(const VkrEditorScripts *scripts) {
  return scripts->open && scripts->first_build &&
         (scripts->status == VKR_EDITOR_SCRIPT_UNBUILT ||
          scripts->status == VKR_EDITOR_SCRIPT_BUILDING ||
          scripts->status == VKR_EDITOR_SCRIPT_LOADING);
}

void vkr_editor_scripts_close_project(VkrEditorScripts *scripts,
                                      EditorBakeryService *service,
                                      const VkrSampleUiFrame *frame) {
  if (!scripts->open) {
    return;
  }
  scripts_join_worker(scripts);
  if (service) {
    for (uint32_t i = 0; i < scripts->module_count; ++i) {
      if (scripts->modules[i].watch) {
        editor_bakery_service_unwatch(service, scripts->modules[i].watch);
      }
    }
    if (scripts->folder_watch) {
      editor_bakery_service_unwatch(service, scripts->folder_watch);
    }
  }
  if (frame->script_request) {
    frame->script_request->retire_libraries = true_v;
  }
  scripts->open = false_v;
  scripts->first_build = false_v;
  scripts->module_count = 0u;
  scripts->file_count = 0u;
  scripts->diagnostic_count = 0u;
  scripts->folder_watch = 0u;
  scripts->rebuild = false_v;
  scripts->revision++;
}

void vkr_editor_scripts_rebuild_file(VkrEditorScripts *scripts,
                                     const char *path) {
  if (scripts_index_of(scripts, path) != SCRIPTS_NONE) {
    scripts->rebuild = true_v;
  }
}

void vkr_editor_scripts_update(VkrEditorScripts *scripts,
                               EditorBakeryService *service,
                               const VkrSampleUiFrame *frame) {
  if (!scripts->open) {
    return;
  }
  /* Load outcomes the runtime applied since the last frame. */
  for (uint32_t r = 0; r < frame->script_result_count; ++r) {
    const VkrSampleScriptResult *result = &frame->script_results[r];
    if (result->serial <= scripts->result_serial ||
        strcmp(result->name, SCRIPTS_PROJECT_LIBRARY)) {
      continue;
    }
    scripts->result_serial = result->serial;
    if (result->result == VKR_SCRIPT_RELOAD_FAILED) {
      scripts_set_status(scripts, VKR_EDITOR_SCRIPT_LOAD_FAILED,
                         result->message);
      continue;
    }
    scripts->loaded_fingerprint = scripts->pending_fingerprint;
    scripts->load_serial++;
    scripts_set_status(
        scripts, VKR_EDITOR_SCRIPT_LOADED,
        result->result == VKR_SCRIPT_RELOAD_LOADED ? "Loaded"
        : result->result == VKR_SCRIPT_RELOAD_RESTARTED
            ? "Reloaded; its state changed, so the simulation restarted"
            : "Reloaded; state kept");
    log_info("Scripts: %s", scripts->message);
  }

  /* Sources changed on disk, including saves from another editor. */
  if (service) {
    char changed[4][EDITOR_BAKERY_SERVICE_PATH];
    if (scripts->folder_watch &&
        editor_bakery_service_take_changes(service, scripts->folder_watch,
                                           changed, ArrayCount(changed))) {
      scripts_scan(scripts, service);
      scripts->rebuild = true_v;
    }
    for (uint32_t i = 0; i < scripts->module_count; ++i) {
      VkrEditorScriptModule *module = &scripts->modules[i];
      if (module->watch &&
          editor_bakery_service_take_changes(service, module->watch, changed,
                                             ArrayCount(changed))) {
        scripts->rebuild = true_v;
      }
    }
  }

  if (scripts->worker_live &&
      vkr_atomic_bool_load(&scripts->worker_done, VKR_MEMORY_ORDER_ACQUIRE)) {
    scripts_join_worker(scripts);
    scripts_build_finished(scripts, scripts->process_ok, scripts->exit_code,
                           scripts->worker_build.events, frame);
  }
  if (!scripts->worker_live && scripts->rebuild && scripts->module_count) {
    (void)scripts_start_build(scripts);
  }
}

uint32_t vkr_editor_scripts_header_folders(const VkrEditorScripts *scripts,
                                           const char *path, const char **out,
                                           uint32_t capacity) {
  const uint32_t owner =
      scripts ? scripts_index_of(scripts, path) : SCRIPTS_NONE;
  if (owner == SCRIPTS_NONE) {
    return 0u;
  }
  /* The owner, then dependencies breadth first, each once. */
  uint32_t order[VKR_EDITOR_SCRIPT_MODULE_MAX];
  uint32_t count = 0u;
  order[count++] = owner;
  for (uint32_t i = 0; i < count; ++i) {
    const VkrEditorScriptModule *module = &scripts->modules[order[i]];
    for (uint32_t d = 0; d < module->dependency_count; ++d) {
      for (uint32_t m = 0; m < scripts->module_count; ++m) {
        bool8_t seen = false_v;
        for (uint32_t k = 0; k < count && !seen; ++k) {
          seen = order[k] == m;
        }
        if (!seen && count < ArrayCount(order) &&
            !strcmp(scripts->modules[m].name, module->dependencies[d])) {
          order[count++] = m;
        }
      }
    }
  }
  for (uint32_t i = 0; i < count && i < capacity; ++i) {
    out[i] = scripts->modules[order[i]].directory;
  }
  return Min(count, capacity);
}

// =============================================================================
// New module template
// =============================================================================

static const char s_template[] =
    "/* {Name}: a C script module (ADR-079). Save to rebuild and hot reload.\n"
    " * Drag this script onto an object, or pick it in Details > Script: the\n"
    " * `{name}` component then runs the hooks below for that object. */\n"
    "#include \"sdk.h\"\n"
    "\n"
    "/* Values edited in Details, listed once: kind, name, label, default and\n"
    " * options such as .unit, .min, .max and .step. */\n"
    "#define {NAME}_FIELDS \\\n"
    "  VKR_FIELD(F32, speed, \"Speed\", 1.0f, .min = 0.0f, .max = 10.0f)\n"
    "VKR_COMPONENT({Name}, {name}, \"{Name}\", {NAME}_FIELDS)\n"
    "\n"
    "/* The object starts playing. What this hook spawns or creates is\n"
    " * released when the object stops. */\n"
    "static void {name}_start(VkrCtx *ctx, VkrEntity self, {Name} *{name}) {\n"
    "}\n"
    "\n"
    "/* Every frame, before the scene advances. */\n"
    "static void {name}_update(VkrCtx *ctx, VkrEntity self, {Name} *{name},\n"
    "                          float32_t dt) {\n"
    "}\n"
    "\n"
    "/* Every fixed physics tick, before physics steps. */\n"
    "static void {name}_fixed_update(VkrCtx *ctx, VkrEntity self,\n"
    "                                {Name} *{name}) {\n"
    "}\n"
    "\n"
    "/* The object is being destroyed; it and what it spawned still exist.\n"
    " * stop follows. */\n"
    "static void {name}_destroy(VkrCtx *ctx, VkrEntity self, {Name} *{name}) "
    "{\n"
    "}\n"
    "\n"
    "/* The object stops playing, loses this script or is destroyed. */\n"
    "static void {name}_stop(VkrCtx *ctx, VkrEntity self, {Name} *{name}) {\n"
    "}\n"
    "\n"
    "VKR_BEHAVIOR({name}, .start = {name}_start, .update = {name}_update,\n"
    "             .fixed_update = {name}_fixed_update,\n"
    "             .destroy = {name}_destroy, .stop = {name}_stop)\n"
    "\n"
    "VKR_MODULE({Name}, VkrNoData, VKR_EXPORT_BEHAVIOR({name}))\n";

/* The template with {Name}, {name} and {NAME} replaced. */
static bool8_t scripts_expand(const char *name, const char *lower,
                              const char *upper, char *out, uint64_t capacity) {
  uint64_t length = 0u;
  for (const char *at = s_template; *at;) {
    const char *value = NULL;
    uint32_t skip = 0u;
    if (!strncmp(at, "{Name}", 6)) {
      value = name;
      skip = 6u;
    } else if (!strncmp(at, "{name}", 6)) {
      value = lower;
      skip = 6u;
    } else if (!strncmp(at, "{NAME}", 6)) {
      value = upper;
      skip = 6u;
    }
    const uint64_t count = value ? strlen(value) : 1u;
    if (length + count + 1u > capacity) {
      return false_v;
    }
    MemCopy(out + length, value ? value : at, count);
    length += count;
    at += value ? skip : 1u;
  }
  out[length] = '\0';
  return true_v;
}

bool8_t vkr_editor_scripts_create_module(VkrEditorScripts *scripts,
                                         const char *name, char *out_path,
                                         uint32_t out_capacity, char *error,
                                         uint32_t error_capacity) {
  const char *failure = NULL;
  char type[VKR_EDITOR_SCRIPT_NAME];
  char upper[VKR_EDITOR_SCRIPT_NAME];
  if (!scripts->open) {
    failure = "Open a project first";
  } else if (!name || !scripts_name_valid(name)) {
    failure = "Use a C identifier: letters, digits and underscores";
  }
  for (uint32_t i = 0; !failure && i < scripts->module_count; ++i) {
    if (!strcmp(scripts->modules[i].name, name)) {
      failure = "A script module with that name exists";
    }
  }
  if (!failure) {
    const size_t length = strlen(name);
    for (size_t i = 0; i <= length; ++i) {
      type[i] = (char)tolower((unsigned char)name[i]);
      upper[i] = (char)toupper((unsigned char)name[i]);
    }
    if (vkr_scene_world_type_named(
            string8_create_from_cstr((const uint8_t *)type, strlen(type)))) {
      failure = "A component type with that name exists";
    }
  }
  char directory[VKR_EDITOR_SCRIPT_PATH];
  char description[VKR_EDITOR_SCRIPT_PATH];
  char source[VKR_EDITOR_SCRIPT_PATH];
  char file_name[VKR_EDITOR_SCRIPT_NAME + 16u];
  if (!failure) {
    snprintf(file_name, sizeof(file_name), "%s.script.json", name);
    if (!scripts_join(directory, scripts->scripts_directory, name) ||
        !scripts_join(description, directory, file_name)) {
      failure = "The script path is too long";
    }
    snprintf(file_name, sizeof(file_name), "%s.c", type);
    if (!failure && !scripts_join(source, directory, file_name)) {
      failure = "The script path is too long";
    }
  }
  if (!failure && !scripts_make_directory(scripts->allocator, directory)) {
    failure = "Cannot create the script folder";
  }
  if (!failure) {
    char json[256];
    snprintf(json, sizeof(json),
             "{\n  \"language\": \"c\",\n  \"sources\": [\"%s.c\"],\n"
             "  \"standard\": \"c11\"\n}\n",
             type);
    const uint64_t capacity = sizeof(s_template) + 16u * 64u;
    char *text = vkr_allocator_alloc(scripts->allocator, capacity,
                                     VKR_ALLOCATOR_MEMORY_TAG_STRING);
    if (!text) {
      failure = "Out of memory";
    } else {
      if (!scripts_expand(name, type, upper, text, capacity) ||
          !scripts_write(description, json) || !scripts_write(source, text)) {
        failure = "Cannot write the script files";
      }
      vkr_allocator_free(scripts->allocator, text, capacity,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
  }
  if (failure) {
    snprintf(error, error_capacity, "%s", failure);
    return false_v;
  }
  /* List and build it now rather than waiting for the folder watch. */
  scripts_scan(scripts, scripts->service);
  if (scripts_index_of(scripts, source) != SCRIPTS_NONE) {
    scripts->rebuild = true_v;
  }
  snprintf(out_path, out_capacity, "%s", source);
  return true_v;
}

// =============================================================================
// Queries
// =============================================================================

bool8_t vkr_editor_scripts_project_open(const VkrEditorScripts *scripts) {
  return scripts && scripts->open;
}

uint64_t vkr_editor_scripts_load_serial(const VkrEditorScripts *scripts) {
  return scripts ? scripts->load_serial : 0u;
}

uint64_t vkr_editor_scripts_revision(const VkrEditorScripts *scripts) {
  return scripts ? scripts->revision : 0u;
}

uint32_t vkr_editor_scripts_module_count(const VkrEditorScripts *scripts) {
  return scripts ? scripts->module_count : 0u;
}

const VkrEditorScriptModule *
vkr_editor_scripts_module(const VkrEditorScripts *scripts, uint32_t index) {
  return scripts && index < scripts->module_count ? &scripts->modules[index]
                                                  : NULL;
}

uint32_t vkr_editor_scripts_file_count(const VkrEditorScripts *scripts) {
  return scripts ? scripts->file_count : 0u;
}

const VkrEditorScriptFile *
vkr_editor_scripts_file(const VkrEditorScripts *scripts, uint32_t index) {
  return scripts && index < scripts->file_count ? &scripts->files[index] : NULL;
}

const VkrEditorScriptModule *
vkr_editor_scripts_module_of(const VkrEditorScripts *scripts,
                             const char *path) {
  const uint32_t index =
      scripts ? scripts_index_of(scripts, path) : SCRIPTS_NONE;
  return index == SCRIPTS_NONE ? NULL : &scripts->modules[index];
}

uint32_t vkr_editor_scripts_diagnostic_count(const VkrEditorScripts *scripts) {
  return scripts ? scripts->diagnostic_count : 0u;
}

const VkrEditorScriptDiagnostic *
vkr_editor_scripts_diagnostic(const VkrEditorScripts *scripts, uint32_t index) {
  return scripts && index < scripts->diagnostic_count
             ? &scripts->diagnostics[index]
             : NULL;
}
