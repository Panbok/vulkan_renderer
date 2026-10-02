/* Script producers (ADR-077, ADR-079). A C script package is described by
 * `<name>.script.json`:
 *
 *   {"language": "c", "sources": ["behavior.c"], "include_roots": ["include"],
 *    "defines": {"SPEED": "2"}, "standard": "c11", "kind": "module",
 *    "dependencies": ["Common"]}
 *
 * with paths relative to the description. A `module` defines
 * `vkr_module_<name>`; a `library` has no entry point and serves the
 * packages that list it in `dependencies`, which compile with its include
 * roots (or its folder). Each translation unit compiles in its own cached
 * `script_object` action whose depfile adds the headers it read; one
 * `script_library` action links the hot-reload library (`lib<name>.dylib`,
 * `<name>.dll`) and archives the objects for static linking at bundle time
 * (`lib<name>.a`, `<name>.lib`).
 *
 * `cook <name>.script.json` builds one package without dependencies into its
 * own library. `scripts <folder> --name <project>` builds every package of a
 * Scripts folder into one project library, with a generated
 * `<project>_modules.c` whose `vkr_project_modules` lists the modules. */

#include "vkr_bakery_internal.h"

#include "vkr_bakery_identity.h"
#include "vkr_bakery_os.h"

#include "platform/vkr_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VKR_SCRIPT_MAX_SOURCES 256u

// =============================================================================
// Toolchain
// =============================================================================

#if defined(_WIN32)
#define VKR_SCRIPT_COMPILER "clang-cl"
#define VKR_SCRIPT_LINKER "lld-link"
#define VKR_SCRIPT_ARCHIVER "llvm-lib"
#else
#define VKR_SCRIPT_DRIVER "xcrun"
#endif

#if !defined(VKR_BAKERY_SCRIPT_SDK_DIRS)
#define VKR_BAKERY_SCRIPT_SDK_DIRS ""
#endif

/* The engine header directories every script compiles against (ADR-079),
 * `|`-separated in the build definition. They join each object's recipe, so
 * moving the engine rebuilds. */
vkr_internal VkrBakeryJson *vkr_script_sdk_roots(Arena *arena) {
  VkrBakeryJson *roots = vkr_bakery_json_array(arena);
  const char *cursor = VKR_BAKERY_SCRIPT_SDK_DIRS;
  while (*cursor) {
    const char *end = strchr(cursor, '|');
    const size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
    char directory[VKR_BAKERY_PATH_CAPACITY];
    if (length && length < sizeof(directory)) {
      MemCopy(directory, cursor, length);
      directory[length] = '\0';
      vkr_bakery_json_append(roots, vkr_bakery_json_cstr(arena, directory));
    }
    cursor += length + (end ? 1u : 0u);
  }
  return roots;
}

/* The compiler's version line keys every script action, so a toolchain
 * update rebuilds. Planning runs on the main thread; the probe runs once. */
vkr_internal const char *vkr_script_compiler_version(VkrBakeryGraph *graph) {
  static bool8_t probed = false_v;
  static char version[256];
  if (probed) {
    return version[0] ? version : NULL;
  }
  probed = true_v;
  char log[VKR_BAKERY_PATH_CAPACITY];
  (void)vkr_bakery_make_directories(graph->config->cache_dir);
  vkr_bakery_temp_path(log, sizeof(log), graph->config->cache_dir,
                       "script-compiler");
#if defined(_WIN32)
  const char *executable = VKR_SCRIPT_COMPILER;
  const char *arguments[] = {"--version"};
#else
  const char *executable = VKR_SCRIPT_DRIVER;
  const char *arguments[] = {"clang", "--version"};
#endif
  const VkrPlatformProcessConfig config = {
      .executable = executable,
      .arguments = arguments,
      .argument_count = ArrayCount(arguments),
      .stdout_path = log,
      .stderr_path = log,
      .append_output = true_v,
      .timeout_ms = 30000u,
      .termination_grace_ms = 100u,
      .hidden = true_v,
  };
  int32_t exit_code = -1;
  bool8_t timed_out = false_v;
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (vkr_platform_process_run(&config, &exit_code, &timed_out) &&
      exit_code == 0 && vkr_bakery_read_file(log, KB(64), &data, &length)) {
    char *end = strpbrk((char *)data, "\r\n");
    if (end) {
      *end = 0;
    }
    (void)snprintf(version, sizeof(version), "%s", (char *)data);
  }
  free(data);
  (void)vkr_bakery_remove_file(log);
  return version[0] ? version : NULL;
}

// =============================================================================
// script_object: one translation unit
// =============================================================================

vkr_internal const char *const vkr_script_object_fields[] = {
    "module",        "language", "standard",  "defines",
    "include_roots", "compiler", "sdk_roots", NULL};

vkr_internal bool8_t vkr_script_object_plan(VkrBakeryGraph *graph,
                                            VkrBakeryAction *action) {
  if (!action->source || !vkr_bakery_action_input(
                             graph, action, action->source,
                             vkr_bakery_graph_display(graph, action->source))) {
    return false_v;
  }
  vkr_bakery_action_label(graph, action, "cc %s",
                          vkr_bakery_path_name(action->source));
  return true_v;
}

vkr_internal bool8_t vkr_script_object_run(VkrBakeryTask *task) {
  const VkrBakeryAction *action = task->action;
  const VkrBakeryJson *recipe = action->recipe;
  const char *staged = vkr_bakery_task_stage(task, "unit.o");
  const char *depfile = vkr_bakery_task_stage(task, "unit.d");
  const char *arguments[60];
  uint32_t count = 0u;
  char standard[40];
  char defines[24][300];
  char includes[16][VKR_BAKERY_PATH_CAPACITY + 4u];
  uint32_t define_count = 0u;
  uint32_t include_count = 0u;
#if defined(_WIN32)
  char depfile_argument[VKR_BAKERY_PATH_CAPACITY + 16u];
  char object_argument[VKR_BAKERY_PATH_CAPACITY + 8u];
  (void)snprintf(standard, sizeof(standard), "/std:%s",
                 vkr_bakery_recipe_string(recipe, "standard", "c11"));
  arguments[count++] = "/nologo";
  arguments[count++] = "/c";
  arguments[count++] = "/O2";
  arguments[count++] = "/W3";
  arguments[count++] = standard;
  const char *system_flag = "/imsvc";
#else
  (void)snprintf(standard, sizeof(standard), "-std=%s",
                 vkr_bakery_recipe_string(recipe, "standard", "c11"));
  arguments[count++] = "clang";
  arguments[count++] = "-x";
  arguments[count++] = "c";
  arguments[count++] = standard;
  arguments[count++] = "-O2";
  arguments[count++] = "-fPIC";
  arguments[count++] = "-fvisibility=hidden";
  arguments[count++] = "-Wall";
  arguments[count++] = "-fno-color-diagnostics";
  const char *system_flag = "-isystem";
#endif
  const VkrBakeryJson *define_object = vkr_bakery_json_get(recipe, "defines");
  for (const VkrBakeryJson *define = define_object ? define_object->first
                                                   : NULL;
       define && define_count < ArrayCount(defines); define = define->next) {
    if (define->type == VKR_BAKERY_JSON_STRING) {
      (void)snprintf(defines[define_count], sizeof(defines[0]), "-D%.*s=%.*s",
                     (int)define->key.length, (const char *)define->key.str,
                     (int)define->string.length,
                     (const char *)define->string.str);
    } else {
      (void)snprintf(defines[define_count], sizeof(defines[0]), "-D%.*s=%lld",
                     (int)define->key.length, (const char *)define->key.str,
                     (long long)define->integer);
    }
    arguments[count++] = defines[define_count++];
  }
  const VkrBakeryJson *roots = vkr_bakery_json_get(recipe, "include_roots");
  for (const VkrBakeryJson *root = roots ? roots->first : NULL;
       root && include_count < ArrayCount(includes); root = root->next) {
    char absolute[VKR_BAKERY_PATH_CAPACITY];
    const char *name = (const char *)root->string.str;
    if (vkr_bakery_path_is_absolute(name) ||
        !vkr_bakery_path_join(absolute, sizeof(absolute), task->config->root,
                              name)) {
      (void)snprintf(absolute, sizeof(absolute), "%s", name);
    }
    (void)snprintf(includes[include_count], sizeof(includes[0]), "-I%s",
                   absolute);
    arguments[count++] = includes[include_count++];
  }
  /* Engine headers are system includes: their own warnings are not the
     script's diagnostics. */
  const VkrBakeryJson *sdk = vkr_bakery_json_get(recipe, "sdk_roots");
  for (const VkrBakeryJson *root = sdk ? sdk->first : NULL;
       root && count + 2u < ArrayCount(arguments); root = root->next) {
    arguments[count++] = system_flag;
    arguments[count++] = (const char *)root->string.str;
  }
#if defined(_WIN32)
  (void)snprintf(depfile_argument, sizeof(depfile_argument), "/clang:-MF%s",
                 depfile);
  (void)snprintf(object_argument, sizeof(object_argument), "/Fo%s", staged);
  arguments[count++] = "/clang:-MD";
  arguments[count++] = depfile_argument;
  arguments[count++] = action->source;
  arguments[count++] = object_argument;
  const char *executable = VKR_SCRIPT_COMPILER;
#else
  arguments[count++] = "-MD";
  arguments[count++] = "-MF";
  arguments[count++] = depfile;
  arguments[count++] = "-c";
  arguments[count++] = action->source;
  arguments[count++] = "-o";
  arguments[count++] = staged;
  const char *executable = VKR_SCRIPT_DRIVER;
#endif
  int32_t exit_code = -1;
  if (!vkr_bakery_task_process(task, executable, arguments, count,
                               &exit_code)) {
    return false_v;
  }
  (void)vkr_bakery_task_parse_compiler_diags(task, VKR_BAKERY_DIAG_SCRIPT_ERROR,
                                             VKR_BAKERY_DIAG_SCRIPT_WARNING);
  if (exit_code != 0) {
    vkr_bakery_task_fail(task, "the C compiler exited with %d", exit_code);
    return false_v;
  }
  vkr_bakery_task_read_depfile(task, depfile);
  return vkr_bakery_task_product(task, "object", staged);
}

const VkrBakeryProducer vkr_bakery_producer_script_object = {
    .id = "script_object",
    .version = 1u,
    .identity = VKR_BAKERY_IDENTITY_SCRIPT,
    .summary = "One C script translation unit to an object file.",
    .recipe_fields = vkr_script_object_fields,
    .plan = vkr_script_object_plan,
    .run = vkr_script_object_run,
};

// =============================================================================
// script_library: hot-reload library and static archive
// =============================================================================

vkr_internal const char *const vkr_script_library_fields[] = {
    "module", "compiler", "platform", NULL};

vkr_internal bool8_t vkr_script_library_plan(VkrBakeryGraph *graph,
                                             VkrBakeryAction *action) {
  const char *module = vkr_bakery_recipe_string(action->recipe, "module", "");
  action->display = vkr_bakery_graph_printf(graph, "scripts/%s", module);
  vkr_bakery_action_label(graph, action, "link + archive");
  return true_v;
}

/* Writes the object paths one per line for the linker's response file. */
vkr_internal bool8_t vkr_script_objects_file(VkrBakeryTask *task,
                                             const char *path) {
  const VkrBakeryAction *action = task->action;
  char *text = NULL;
  uint64_t length = 0u;
  for (uint32_t i = 0u; i < action->input_count; ++i) {
    const VkrBakeryInput *input = &action->inputs[i];
    if (input->dep_action < 0 || !input->path) {
      continue;
    }
    const uint64_t size = strlen(input->path);
    char *grown = realloc(text, length + size + 4u);
    if (!grown) {
      free(text);
      return false_v;
    }
    text = grown;
#if defined(_WIN32)
    text[length++] = '"';
    MemCopy(text + length, input->path, size);
    length += size;
    text[length++] = '"';
#else
    MemCopy(text + length, input->path, size);
    length += size;
#endif
    text[length++] = '\n';
  }
  const bool8_t ok = text && vkr_bakery_write_file_atomic(path, text, length);
  free(text);
  return ok;
}

vkr_internal bool8_t vkr_script_library_run(VkrBakeryTask *task) {
  const char *module =
      vkr_bakery_recipe_string(task->action->recipe, "module", "script");
  const char *objects = vkr_bakery_task_stage(task, "objects.txt");
  if (!vkr_script_objects_file(task, objects)) {
    vkr_bakery_task_fail(task, "cannot list the module's objects");
    return false_v;
  }
  char response[VKR_BAKERY_PATH_CAPACITY + 2u];
  (void)snprintf(response, sizeof(response), "@%s", objects);
  int32_t exit_code = -1;
#if defined(_WIN32)
  const char *library = vkr_bakery_task_stage(task, "module.dll");
  const char *archive = vkr_bakery_task_stage(task, "module.lib");
  char library_argument[VKR_BAKERY_PATH_CAPACITY + 8u];
  char archive_argument[VKR_BAKERY_PATH_CAPACITY + 8u];
  (void)snprintf(library_argument, sizeof(library_argument), "/OUT:%s",
                 library);
  (void)snprintf(archive_argument, sizeof(archive_argument), "/OUT:%s",
                 archive);
  /* The default DLL entry initializes the module's own static C runtime,
     which SDK calls such as vkr_log use. */
  const char *link[] = {"/NOLOGO", "/DLL", library_argument, response};
  const char *pack[] = {"/NOLOGO", archive_argument, response};
  const char *linker = VKR_SCRIPT_LINKER;
  const char *archiver = VKR_SCRIPT_ARCHIVER;
#else
  const char *library = vkr_bakery_task_stage(task, "module.dylib");
  const char *archive = vkr_bakery_task_stage(task, "module.a");
  char install_name[300];
  (void)snprintf(install_name, sizeof(install_name), "@rpath/lib%s.dylib",
                 module);
  /* Engine symbols resolve against the host that loads the library. */
  const char *link[] = {
      "clang",         "-dynamiclib", "-undefined", "dynamic_lookup",
      "-install_name", install_name,  response,     "-o",
      library};
  /* -D zeroes member dates and ids so equal objects archive identically. */
  const char *pack[] = {
      "libtool", "-static", "-D",        "-no_warning_for_no_symbols",
      "-o",      archive,   "-filelist", objects};
  const char *linker = VKR_SCRIPT_DRIVER;
  const char *archiver = VKR_SCRIPT_DRIVER;
#endif
  if (!vkr_bakery_task_process(task, linker, link, ArrayCount(link),
                               &exit_code)) {
    return false_v;
  }
  (void)vkr_bakery_task_parse_compiler_diags(task, VKR_BAKERY_DIAG_SCRIPT_ERROR,
                                             VKR_BAKERY_DIAG_SCRIPT_WARNING);
  if (exit_code != 0) {
    vkr_bakery_task_fail(task, "linking %s exited with %d", module, exit_code);
    return false_v;
  }
  if (!vkr_bakery_task_process(task, archiver, pack, ArrayCount(pack),
                               &exit_code)) {
    return false_v;
  }
  if (exit_code != 0) {
    vkr_bakery_task_fail(task, "archiving %s exited with %d", module,
                         exit_code);
    return false_v;
  }
  return vkr_bakery_task_product(task, "library", library) &&
         vkr_bakery_task_product(task, "archive", archive);
}

const VkrBakeryProducer vkr_bakery_producer_script_library = {
    .id = "script_library",
    .version = 1u,
    .identity = VKR_BAKERY_IDENTITY_SCRIPT,
    .summary = "A C script module's hot-reload library and static archive.",
    .recipe_fields = vkr_script_library_fields,
    .plan = vkr_script_library_plan,
    .run = vkr_script_library_run,
};

// =============================================================================
// Package planning
// =============================================================================

/* Packages one Scripts folder may hold. */
#define VKR_SCRIPT_MAX_PACKAGES 256u

typedef enum VkrScriptKind {
  VKR_SCRIPT_KIND_MODULE = 0,
  /* Code other packages link and include; no entry point. */
  VKR_SCRIPT_KIND_LIBRARY,
} VkrScriptKind;

/* One `<name>.script.json` package, read and validated. */
typedef struct VkrScriptPackage {
  char name[128];
  const char *description;
  char directory[VKR_BAKERY_PATH_CAPACITY];
  VkrScriptKind kind;
  const VkrBakeryJson *sources;
  const VkrBakeryJson *defines;
  const VkrBakeryJson *standard;
  const VkrBakeryJson *dependencies;
  /* Declared include roots as portable display names. */
  VkrBakeryJson *roots;
  /* What dependents include: the declared roots, else the package folder. */
  VkrBakeryJson *public_roots;
} VkrScriptPackage;

vkr_internal bool8_t vkr_script_fail(VkrBakeryGraph *graph,
                                     const char *description,
                                     VkrBakeryDiag diag, const char *message) {
  vkr_bakery_event_diag(0u, diag, vkr_bakery_graph_display(graph, description),
                        0u, 0u, message, NULL);
  graph->plan_failed = true_v;
  return false_v;
}

/* `<module>` of `<directory>/<module>.script.json`. */
vkr_internal bool8_t vkr_script_module_name(const char *description, char *out,
                                            uint32_t capacity) {
  const char *name = vkr_bakery_path_name(description);
  const char *suffix = ".script.json";
  const uint64_t length = strlen(name);
  const uint64_t suffix_length = strlen(suffix);
  if (length <= suffix_length ||
      strcmp(name + length - suffix_length, suffix) != 0 ||
      length - suffix_length >= capacity) {
    return false_v;
  }
  MemCopy(out, name, length - suffix_length);
  out[length - suffix_length] = 0;
  for (char *c = out; *c; ++c) {
    const bool8_t valid = (*c >= 'a' && *c <= 'z') ||
                          (*c >= 'A' && *c <= 'Z') ||
                          (*c >= '0' && *c <= '9') || *c == '_' || *c == '-';
    if (!valid) {
      return false_v;
    }
  }
  return true_v;
}

/* A module's entry `vkr_module_<name>` needs a C identifier. */
vkr_internal bool8_t vkr_script_identifier(const char *name) {
  if (!name[0] || (name[0] >= '0' && name[0] <= '9')) {
    return false_v;
  }
  for (const char *c = name; *c; ++c) {
    if (*c == '-') {
      return false_v;
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_script_key_is(const VkrBakeryJson *field,
                                       const char *key) {
  return field->key.length == strlen(key) &&
         MemCompare(field->key.str, key, field->key.length) == 0;
}

/* Reads and validates a package description. */
vkr_internal bool8_t vkr_script_package_read(VkrBakeryGraph *graph,
                                             const char *description,
                                             VkrScriptPackage *package) {
  Arena *arena = graph->arena;
  *package = (VkrScriptPackage){.description = description};
  if (!vkr_script_module_name(description, package->name,
                              sizeof(package->name))) {
    return vkr_script_fail(graph, description,
                           VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                           "a script package is <name>.script.json with a "
                           "name of letters, digits, '_' and '-'");
  }
  uint8_t *data = NULL;
  uint64_t length = 0u;
  VkrBakeryJsonError error = {0};
  const VkrBakeryJson *document = NULL;
  if (vkr_bakery_read_file(description, MB(1), &data, &length)) {
    document = vkr_bakery_json_parse(arena, data, length, 16u, &error);
  }
  free(data);
  if (!document || document->type != VKR_BAKERY_JSON_OBJECT) {
    vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_REC_UNREADABLE,
                          vkr_bakery_graph_display(graph, description),
                          error.line, error.column,
                          error.message[0] ? error.message : NULL, NULL);
    graph->plan_failed = true_v;
    return false_v;
  }
  static const char *const fields[] = {
      "language", "sources", "include_roots", "defines",
      "standard", "kind",    "dependencies"};
  for (const VkrBakeryJson *field = document->first; field;
       field = field->next) {
    bool8_t known = false_v;
    for (uint32_t i = 0u; i < ArrayCount(fields) && !known; ++i) {
      known = vkr_script_key_is(field, fields[i]);
    }
    if (!known) {
      char key[64];
      (void)snprintf(key, sizeof(key), "%.*s", (int)field->key.length,
                     (const char *)field->key.str);
      vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_REC_UNKNOWN_FIELD,
                            vkr_bakery_graph_display(graph, description), 0u,
                            0u, key, NULL);
      graph->plan_failed = true_v;
      return false_v;
    }
  }
  if (!vkr_bakery_json_is_string(vkr_bakery_json_get(document, "language"),
                                 "c")) {
    return vkr_script_fail(graph, description,
                           VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                           "language must be \"c\"; \"js\" and \"ts\" are "
                           "reserved");
  }
  const VkrBakeryJson *kind = vkr_bakery_json_get(document, "kind");
  if (kind && vkr_bakery_json_is_string(kind, "library")) {
    package->kind = VKR_SCRIPT_KIND_LIBRARY;
  } else if (kind && !vkr_bakery_json_is_string(kind, "module")) {
    return vkr_script_fail(graph, description,
                           VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                           "kind must be \"module\" or \"library\"");
  }
  if (package->kind == VKR_SCRIPT_KIND_MODULE &&
      !vkr_script_identifier(package->name)) {
    return vkr_script_fail(graph, description,
                           VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                           "a module's name must be a C identifier");
  }
  /* A library may be headers only; a module compiles at least one file. */
  package->sources = vkr_bakery_json_get(document, "sources");
  const uint32_t minimum = package->kind == VKR_SCRIPT_KIND_LIBRARY ? 0u : 1u;
  if (!package->sources || package->sources->type != VKR_BAKERY_JSON_ARRAY ||
      package->sources->count < minimum ||
      package->sources->count > VKR_SCRIPT_MAX_SOURCES) {
    return vkr_script_fail(graph, description,
                           VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                           "sources must list 1 to 256 C files");
  }
  package->dependencies = vkr_bakery_json_get(document, "dependencies");
  if (package->dependencies &&
      package->dependencies->type != VKR_BAKERY_JSON_ARRAY) {
    return vkr_script_fail(graph, description,
                           VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                           "dependencies must list package names");
  }
  vkr_bakery_path_parent(package->directory, sizeof(package->directory),
                         description);
  /* Include roots become root-relative names so keys survive checkouts. */
  package->roots = vkr_bakery_json_array(arena);
  const VkrBakeryJson *declared =
      vkr_bakery_json_get(document, "include_roots");
  for (const VkrBakeryJson *root = declared ? declared->first : NULL; root;
       root = root->next) {
    char absolute[VKR_BAKERY_PATH_CAPACITY];
    const char *name = vkr_bakery_json_cstr_value(arena, root);
    if (!name ||
        !vkr_bakery_path_join(absolute, sizeof(absolute), package->directory,
                              name) ||
        !vkr_bakery_is_directory(absolute)) {
      return vkr_script_fail(graph, description,
                             VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                             "an include root is not a directory");
    }
    vkr_bakery_json_append(
        package->roots,
        vkr_bakery_json_cstr(arena, vkr_bakery_graph_display(graph, absolute)));
  }
  package->public_roots =
      package->roots->count ? package->roots : vkr_bakery_json_array(arena);
  if (!package->roots->count) {
    vkr_bakery_json_append(
        package->public_roots,
        vkr_bakery_json_cstr(
            arena, vkr_bakery_graph_display(graph, package->directory)));
  }
  package->defines = vkr_bakery_json_get(document, "defines");
  if (package->defines && package->defines->type != VKR_BAKERY_JSON_OBJECT) {
    return vkr_script_fail(graph, description,
                           VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                           "defines must be an object");
  }
  package->standard = vkr_bakery_json_get(document, "standard");
  return true_v;
}

/* Plans one `script_object` per source of `package`, each an input of
 * `library`, compiled with `roots` on its include path. */
vkr_internal bool8_t vkr_script_package_objects(VkrBakeryGraph *graph,
                                                const VkrScriptPackage *package,
                                                const VkrBakeryJson *roots,
                                                const char *compiler,
                                                VkrBakeryAction *library) {
  Arena *arena = graph->arena;
  for (const VkrBakeryJson *source = package->sources->first; source;
       source = source->next) {
    char absolute[VKR_BAKERY_PATH_CAPACITY];
    const char *name = vkr_bakery_json_cstr_value(arena, source);
    if (!name ||
        !vkr_bakery_path_join(absolute, sizeof(absolute), package->directory,
                              name) ||
        !vkr_bakery_is_file(absolute)) {
      vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_IDX_MISSING_SOURCE,
                            name ? name : "", 0u, 0u,
                            "a script source is missing", NULL);
      graph->plan_failed = true_v;
      return false_v;
    }
    VkrBakeryJson *recipe = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, recipe, "module",
                        vkr_bakery_json_cstr(arena, package->name));
    vkr_bakery_json_set(arena, recipe, "language",
                        vkr_bakery_json_cstr(arena, "c"));
    vkr_bakery_json_set(arena, recipe, "standard",
                        package->standard
                            ? vkr_bakery_json_clone(arena, package->standard)
                            : vkr_bakery_json_cstr(arena, "c11"));
    vkr_bakery_json_set(arena, recipe, "defines",
                        package->defines
                            ? vkr_bakery_json_clone(arena, package->defines)
                            : vkr_bakery_json_object(arena));
    vkr_bakery_json_set(arena, recipe, "include_roots",
                        vkr_bakery_json_clone(arena, roots));
    vkr_bakery_json_set(arena, recipe, "sdk_roots",
                        vkr_script_sdk_roots(arena));
    vkr_bakery_json_set(arena, recipe, "compiler",
                        vkr_bakery_json_cstr(arena, compiler));
    VkrBakeryAction *object = vkr_bakery_graph_add(
        graph, &vkr_bakery_producer_script_object,
        vkr_bakery_graph_strdup(graph, absolute), recipe, NULL);
    if (!object || !vkr_bakery_action_dep_input(
                       graph, library, object, "object",
                       vkr_bakery_graph_display(graph, absolute))) {
      return false_v;
    }
  }
  return true_v;
}

/* The link action of library `name`, with its outputs in `target`. */
vkr_internal VkrBakeryAction *vkr_script_library_action(VkrBakeryGraph *graph,
                                                        const char *name,
                                                        const char *compiler,
                                                        const char *target) {
  Arena *arena = graph->arena;
  VkrBakeryJson *recipe = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, recipe, "module",
                      vkr_bakery_json_cstr(arena, name));
  vkr_bakery_json_set(arena, recipe, "compiler",
                      vkr_bakery_json_cstr(arena, compiler));
  vkr_bakery_json_set(arena, recipe, "platform",
                      vkr_bakery_json_cstr(arena, graph->config->platform));
  VkrBakeryAction *library = vkr_bakery_graph_add(
      graph, &vkr_bakery_producer_script_library, NULL, recipe, NULL);
  if (!library) {
    return NULL;
  }
  char library_path[VKR_BAKERY_PATH_CAPACITY];
  char archive_path[VKR_BAKERY_PATH_CAPACITY];
#if defined(_WIN32)
  (void)snprintf(library_path, sizeof(library_path), "%s/%s.dll", target, name);
  (void)snprintf(archive_path, sizeof(archive_path), "%s/%s.lib", target, name);
#else
  (void)snprintf(library_path, sizeof(library_path), "%s/lib%s.dylib", target,
                 name);
  (void)snprintf(archive_path, sizeof(archive_path), "%s/lib%s.a", target,
                 name);
#endif
  if (!vkr_bakery_action_output(graph, library, "library", library_path) ||
      !vkr_bakery_action_output(graph, library, "archive", archive_path)) {
    return NULL;
  }
  return library;
}

vkr_internal const char *vkr_script_compiler_or_fail(VkrBakeryGraph *graph,
                                                     const char *description) {
  const char *compiler = vkr_script_compiler_version(graph);
  if (!compiler) {
    (void)vkr_script_fail(graph, description,
                          VKR_BAKERY_DIAG_SCRIPT_COMPILER_MISSING,
                          "no C compiler for scripts");
  }
  return compiler;
}

VkrBakeryAction *vkr_bakery_plan_script(VkrBakeryGraph *graph,
                                        const char *description,
                                        const char *output_directory) {
  VkrScriptPackage package;
  if (!vkr_script_package_read(graph, description, &package)) {
    return NULL;
  }
  if (package.dependencies && package.dependencies->count) {
    (void)vkr_script_fail(graph, description, VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                          "a package with dependencies builds with its "
                          "Scripts folder: vkr_bakery scripts <folder>");
    return NULL;
  }
  const char *compiler = vkr_script_compiler_or_fail(graph, description);
  if (!compiler) {
    return NULL;
  }
  VkrBakeryAction *library = vkr_script_library_action(
      graph, package.name, compiler,
      output_directory ? output_directory : package.directory);
  if (!library || !vkr_script_package_objects(graph, &package, package.roots,
                                              compiler, library)) {
    return NULL;
  }
  return library;
}

// =============================================================================
// Project planning
// =============================================================================

typedef struct VkrScriptProject {
  VkrBakeryGraph *graph;
  const char *directory;
  VkrScriptPackage *packages;
  uint32_t count;
  bool8_t failed;
} VkrScriptProject;

/* A folder `<name>` holding `<name>.script.json` is a package. */
vkr_internal bool8_t vkr_script_project_visit(void *context, const char *name,
                                              bool8_t is_directory) {
  VkrScriptProject *project = context;
  char folder[VKR_BAKERY_PATH_CAPACITY];
  char description[VKR_BAKERY_PATH_CAPACITY];
  char file[160];
  (void)snprintf(file, sizeof(file), "%s.script.json", name);
  if (!is_directory ||
      !vkr_bakery_path_join(folder, sizeof(folder), project->directory, name) ||
      !vkr_bakery_path_join(description, sizeof(description), folder, file) ||
      !vkr_bakery_is_file(description)) {
    return true_v;
  }
  if (project->count == VKR_SCRIPT_MAX_PACKAGES) {
    project->failed = !vkr_script_fail(project->graph, description,
                                       VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                                       "a Scripts folder holds at most 256 "
                                       "packages");
    return false_v;
  }
  VkrScriptPackage *package = &project->packages[project->count];
  if (!vkr_script_package_read(
          project->graph, vkr_bakery_graph_strdup(project->graph, description),
          package)) {
    project->failed = true_v;
    return true_v;
  }
  project->count++;
  return true_v;
}

vkr_internal int32_t vkr_script_project_find(const VkrScriptProject *project,
                                             const char *name,
                                             uint64_t length) {
  for (uint32_t i = 0u; i < project->count; ++i) {
    if (strlen(project->packages[i].name) == length &&
        MemCompare(project->packages[i].name, name, length) == 0) {
      return (int32_t)i;
    }
  }
  return -1;
}

/* Adds the public roots of `index` and of everything it depends on to
 * `roots`, once each. `state` marks packages: 1 visiting, 2 added. A package
 * met while visiting closes a cycle. */
vkr_internal bool8_t vkr_script_project_roots(VkrScriptProject *project,
                                              uint32_t index, uint8_t *state,
                                              VkrBakeryJson *roots,
                                              bool8_t include_self) {
  VkrBakeryGraph *graph = project->graph;
  const VkrScriptPackage *package = &project->packages[index];
  if (state[index] == 1u) {
    char message[256];
    (void)snprintf(message, sizeof(message),
                   "the dependencies of %s form a cycle", package->name);
    return vkr_script_fail(graph, package->description,
                           VKR_BAKERY_DIAG_REC_INVALID_VALUE, message);
  }
  if (state[index] == 2u) {
    return true_v;
  }
  state[index] = 1u;
  for (const VkrBakeryJson *dependency =
           package->dependencies ? package->dependencies->first : NULL;
       dependency; dependency = dependency->next) {
    const int32_t found =
        dependency->type == VKR_BAKERY_JSON_STRING
            ? vkr_script_project_find(project,
                                      (const char *)dependency->string.str,
                                      dependency->string.length)
            : -1;
    if (found < 0) {
      char message[256];
      (void)snprintf(message, sizeof(message),
                     "%s depends on %.*s, which is not a package here",
                     package->name, (int)dependency->string.length,
                     (const char *)dependency->string.str);
      return vkr_script_fail(graph, package->description,
                             VKR_BAKERY_DIAG_REC_INVALID_VALUE, message);
    }
    if (!vkr_script_project_roots(project, (uint32_t)found, state, roots,
                                  true_v)) {
      return false_v;
    }
  }
  state[index] = 2u;
  if (include_self) {
    for (const VkrBakeryJson *root = package->public_roots->first; root;
         root = root->next) {
      vkr_bakery_json_append(roots, vkr_bakery_json_clone(graph->arena, root));
    }
  }
  return true_v;
}

/* Writes `path` when its bytes differ, so an unchanged module list keeps
 * its object cached. */
vkr_internal bool8_t vkr_script_write_if_changed(const char *path,
                                                 const char *text) {
  const uint64_t length = strlen(text);
  uint8_t *data = NULL;
  uint64_t existing = 0u;
  if (vkr_bakery_read_file(path, MB(1), &data, &existing)) {
    const bool8_t same = existing == length && !MemCompare(data, text, length);
    free(data);
    if (same) {
      return true_v;
    }
  }
  return vkr_bakery_write_file_atomic(path, text, length);
}

/* The project's module list: `vkr_project_modules` returns each module
 * package's `vkr_module_<name>` (sdk.h). */
vkr_internal bool8_t vkr_script_project_entry(const VkrScriptProject *project,
                                              const char *path) {
  char text[64u * 1024u];
  uint64_t length = 0u;
  int written = snprintf(text, sizeof(text),
                         "/* Generated by vkr_bakery scripts: the modules of "
                         "this project (ADR-079). */\n#include \"sdk.h\"\n\n");
  length += written > 0 ? (uint64_t)written : 0u;
  uint32_t modules = 0u;
  for (uint32_t i = 0u; i < project->count && length < sizeof(text); ++i) {
    if (project->packages[i].kind != VKR_SCRIPT_KIND_MODULE) {
      continue;
    }
    written = snprintf(text + length, sizeof(text) - length,
                       "const VkrModuleDesc *vkr_module_%s(uint32_t);\n",
                       project->packages[i].name);
    length += written > 0 ? (uint64_t)written : 0u;
    modules++;
  }
  written = snprintf(text + length, sizeof(text) - length,
                     "\nVKR_SDK_EXPORT uint32_t vkr_project_modules("
                     "VkrModuleEntry *entries,\n"
                     "                                           uint32_t "
                     "capacity) {\n");
  length += written > 0 ? (uint64_t)written : 0u;
  if (modules) {
    written = snprintf(text + length, sizeof(text) - length,
                       "  static const VkrModuleEntry modules[] = {\n");
    length += written > 0 ? (uint64_t)written : 0u;
    for (uint32_t i = 0u; i < project->count && length < sizeof(text); ++i) {
      if (project->packages[i].kind == VKR_SCRIPT_KIND_MODULE) {
        written = snprintf(text + length, sizeof(text) - length,
                           "      vkr_module_%s,\n", project->packages[i].name);
        length += written > 0 ? (uint64_t)written : 0u;
      }
    }
    written =
        snprintf(text + length, sizeof(text) - length,
                 "  };\n"
                 "  for (uint32_t i = 0; i < %uu && i < capacity; ++i) {\n"
                 "    entries[i] = modules[i];\n"
                 "  }\n"
                 "  return %uu;\n}\n",
                 modules, modules);
  } else {
    written =
        snprintf(text + length, sizeof(text) - length,
                 "  (void)entries;\n  (void)capacity;\n  return 0u;\n}\n");
  }
  length += written > 0 ? (uint64_t)written : 0u;
  return length < sizeof(text) && vkr_script_write_if_changed(path, text);
}

VkrBakeryAction *vkr_bakery_plan_script_project(VkrBakeryGraph *graph,
                                                const char *directory,
                                                const char *name,
                                                const char *output_directory) {
  Arena *arena = graph->arena;
  if (!vkr_script_identifier(name) || strlen(name) >= 128u) {
    (void)vkr_script_fail(graph, directory, VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                          "a project library's name must be a C identifier");
    return NULL;
  }
  VkrScriptProject project = {
      .graph = graph,
      .directory = directory,
      .packages =
          arena_alloc(arena, sizeof(VkrScriptPackage) * VKR_SCRIPT_MAX_PACKAGES,
                      ARENA_MEMORY_TAG_ARRAY)};
  if (!project.packages ||
      !vkr_bakery_list_directory(directory, vkr_script_project_visit,
                                 &project) ||
      project.failed) {
    if (!graph->plan_failed) {
      (void)vkr_script_fail(graph, directory, VKR_BAKERY_DIAG_REC_UNREADABLE,
                            "the Scripts folder cannot be read");
    }
    return NULL;
  }
  const char *compiler = vkr_script_compiler_or_fail(graph, directory);
  const char *target = output_directory ? output_directory : directory;
  if (!compiler || !vkr_bakery_make_directories(target)) {
    return NULL;
  }
  VkrBakeryAction *library =
      vkr_script_library_action(graph, name, compiler, target);
  if (!library) {
    return NULL;
  }
  uint8_t *state =
      arena_alloc(arena, project.count + 1u, ARENA_MEMORY_TAG_ARRAY);
  for (uint32_t i = 0u; state && i < project.count; ++i) {
    /* Its own roots, then its dependencies' public roots. */
    MemZero(state, project.count);
    VkrBakeryJson *roots =
        vkr_bakery_json_clone(arena, project.packages[i].roots);
    if (!vkr_script_project_roots(&project, i, state, roots, false_v) ||
        !vkr_script_package_objects(graph, &project.packages[i], roots,
                                    compiler, library)) {
      return NULL;
    }
  }
  /* The generated module list compiles like any project source. */
  char entry[VKR_BAKERY_PATH_CAPACITY];
  (void)snprintf(entry, sizeof(entry), "%s/%s_modules.c", target, name);
  if (!state || !vkr_script_project_entry(&project, entry)) {
    (void)vkr_script_fail(graph, directory, VKR_BAKERY_DIAG_REC_UNREADABLE,
                          "the project's module list cannot be written");
    return NULL;
  }
  VkrScriptPackage list = {.sources = vkr_bakery_json_array(arena),
                           .roots = vkr_bakery_json_array(arena)};
  (void)snprintf(list.name, sizeof(list.name), "%s", name);
  (void)snprintf(list.directory, sizeof(list.directory), "%s", target);
  vkr_bakery_json_append(
      (VkrBakeryJson *)list.sources,
      vkr_bakery_json_cstr(arena, vkr_bakery_path_name(entry)));
  if (!vkr_script_package_objects(graph, &list, list.roots, compiler,
                                  library)) {
    return NULL;
  }
  return library;
}
