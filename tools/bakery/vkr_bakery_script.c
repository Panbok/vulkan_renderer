/* Script producers (ADR-077). A C script module is described by
 * `<module>.script.json`:
 *
 *   {"language": "c", "sources": ["behavior.c"], "include_roots": ["include"],
 *    "defines": {"SPEED": "2"}, "standard": "c11"}
 *
 * with paths relative to the description. Each translation unit compiles in
 * its own cached `script_object` action whose depfile adds the headers it
 * read; one `script_library` action links the editor's hot-reload library
 * (`lib<module>.dylib`, `<module>.dll`) and archives the objects for static
 * linking at bundle time (`lib<module>.a`, `<module>.lib`). Loading and the
 * runtime ABI belong to the entity behavior proposal; these producers only
 * compile, cache, diagnose and publish. */

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
  const char *link[] = {"/NOLOGO", "/DLL", "/NOENTRY", library_argument,
                        response};
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
// Module planning
// =============================================================================

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

VkrBakeryAction *vkr_bakery_plan_script(VkrBakeryGraph *graph,
                                        const char *description,
                                        const char *output_directory) {
  Arena *arena = graph->arena;
  char module[128];
  if (!vkr_script_module_name(description, module, sizeof(module))) {
    (void)vkr_script_fail(graph, description, VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                          "a script module is <name>.script.json with a name "
                          "of letters, digits, '_' and '-'");
    return NULL;
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
    return NULL;
  }
  static const char *const fields[] = {"language", "sources", "include_roots",
                                       "defines", "standard"};
  for (const VkrBakeryJson *field = document->first; field;
       field = field->next) {
    bool8_t known = false_v;
    for (uint32_t i = 0u; i < ArrayCount(fields) && !known; ++i) {
      known = field->key.length == strlen(fields[i]) &&
              MemCompare(field->key.str, fields[i], field->key.length) == 0;
    }
    if (!known) {
      char key[64];
      (void)snprintf(key, sizeof(key), "%.*s", (int)field->key.length,
                     (const char *)field->key.str);
      vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_REC_UNKNOWN_FIELD,
                            vkr_bakery_graph_display(graph, description), 0u,
                            0u, key, NULL);
      graph->plan_failed = true_v;
      return NULL;
    }
  }
  if (!vkr_bakery_json_is_string(vkr_bakery_json_get(document, "language"),
                                 "c")) {
    (void)vkr_script_fail(graph, description, VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                          "language must be \"c\"; \"js\" and \"ts\" are "
                          "reserved");
    return NULL;
  }
  const VkrBakeryJson *sources = vkr_bakery_json_get(document, "sources");
  if (!sources || sources->type != VKR_BAKERY_JSON_ARRAY || !sources->count ||
      sources->count > VKR_SCRIPT_MAX_SOURCES) {
    (void)vkr_script_fail(graph, description, VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                          "sources must list 1 to 256 C files");
    return NULL;
  }
  const char *compiler = vkr_script_compiler_version(graph);
  if (!compiler) {
    (void)vkr_script_fail(graph, description,
                          VKR_BAKERY_DIAG_SCRIPT_COMPILER_MISSING,
                          "no C compiler for scripts");
    return NULL;
  }

  char directory[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_path_parent(directory, sizeof(directory), description);
  /* Include roots become root-relative names so keys survive checkouts. */
  VkrBakeryJson *roots = vkr_bakery_json_array(arena);
  const VkrBakeryJson *declared =
      vkr_bakery_json_get(document, "include_roots");
  for (const VkrBakeryJson *root = declared ? declared->first : NULL; root;
       root = root->next) {
    char absolute[VKR_BAKERY_PATH_CAPACITY];
    const char *name = vkr_bakery_json_cstr_value(arena, root);
    if (!name ||
        !vkr_bakery_path_join(absolute, sizeof(absolute), directory, name) ||
        !vkr_bakery_is_directory(absolute)) {
      (void)vkr_script_fail(graph, description,
                            VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                            "an include root is not a directory");
      return NULL;
    }
    vkr_bakery_json_append(
        roots,
        vkr_bakery_json_cstr(arena, vkr_bakery_graph_display(graph, absolute)));
  }
  const VkrBakeryJson *defines = vkr_bakery_json_get(document, "defines");
  if (defines && defines->type != VKR_BAKERY_JSON_OBJECT) {
    (void)vkr_script_fail(graph, description, VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                          "defines must be an object");
    return NULL;
  }
  const VkrBakeryJson *standard = vkr_bakery_json_get(document, "standard");

  VkrBakeryJson *library_recipe = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, library_recipe, "module",
                      vkr_bakery_json_cstr(arena, module));
  vkr_bakery_json_set(arena, library_recipe, "compiler",
                      vkr_bakery_json_cstr(arena, compiler));
  vkr_bakery_json_set(arena, library_recipe, "platform",
                      vkr_bakery_json_cstr(arena, graph->config->platform));
  VkrBakeryAction *library = vkr_bakery_graph_add(
      graph, &vkr_bakery_producer_script_library, NULL, library_recipe, NULL);
  if (!library) {
    return NULL;
  }
  for (const VkrBakeryJson *source = sources->first; source;
       source = source->next) {
    char absolute[VKR_BAKERY_PATH_CAPACITY];
    const char *name = vkr_bakery_json_cstr_value(arena, source);
    if (!name ||
        !vkr_bakery_path_join(absolute, sizeof(absolute), directory, name) ||
        !vkr_bakery_is_file(absolute)) {
      vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_IDX_MISSING_SOURCE,
                            name ? name : "", 0u, 0u,
                            "a script source is missing", NULL);
      graph->plan_failed = true_v;
      return NULL;
    }
    VkrBakeryJson *recipe = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, recipe, "module",
                        vkr_bakery_json_cstr(arena, module));
    vkr_bakery_json_set(arena, recipe, "language",
                        vkr_bakery_json_cstr(arena, "c"));
    vkr_bakery_json_set(arena, recipe, "standard",
                        standard ? vkr_bakery_json_clone(arena, standard)
                                 : vkr_bakery_json_cstr(arena, "c11"));
    vkr_bakery_json_set(arena, recipe, "defines",
                        defines ? vkr_bakery_json_clone(arena, defines)
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
      return NULL;
    }
  }

  const char *target = output_directory ? output_directory : directory;
  char library_path[VKR_BAKERY_PATH_CAPACITY];
  char archive_path[VKR_BAKERY_PATH_CAPACITY];
#if defined(_WIN32)
  (void)snprintf(library_path, sizeof(library_path), "%s/%s.dll", target,
                 module);
  (void)snprintf(archive_path, sizeof(archive_path), "%s/%s.lib", target,
                 module);
#else
  (void)snprintf(library_path, sizeof(library_path), "%s/lib%s.dylib", target,
                 module);
  (void)snprintf(archive_path, sizeof(archive_path), "%s/lib%s.a", target,
                 module);
#endif
  if (!vkr_bakery_action_output(graph, library, "library", library_path) ||
      !vkr_bakery_action_output(graph, library, "archive", archive_path)) {
    return NULL;
  }
  return library;
}
