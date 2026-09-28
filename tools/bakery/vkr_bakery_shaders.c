#include "vkr_bakery_shaders.h"

#include "platform/vkr_platform.h"
#include "vkr_bakery_buffer.h"
#include "vkr_bakery_identity.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shader compilation as producers (docs/proposals/asset-build-system.md
 * section 9.7). Library recipes beside each backend root list entries and
 * ordered sources; each Vulkan entry, the Metal Slang library, the native MSL
 * concatenation and each metallib is one cached action, and a manifest action
 * per backend publishes the catalog the renderer loads. Outputs must stay
 * byte-identical to the former CMake rules. */

#define VKR_BAKERY_VULKAN_RECIPE                                               \
  "renderer/src/shaders/vulkan/slang/library.recipe.json"
#define VKR_BAKERY_METAL_RECIPE "renderer/src/shaders/metal/library.recipe.json"

// =============================================================================
// Toolchain discovery
// =============================================================================

/* First output line of `executable args...` on stdout or stderr (slangc
 * prints its version on stderr), or false when it cannot run. */
vkr_internal bool8_t vkr_bakery_capture_version(const char *scratch,
                                                const char *executable,
                                                const char *const *arguments,
                                                uint32_t count, char *out,
                                                uint32_t capacity) {
  char log[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_temp_path(log, sizeof(log), scratch, "version");
  const VkrPlatformProcessConfig config = {
      .executable = executable,
      .arguments = arguments,
      .argument_count = count,
      .stdout_path = log,
      .stderr_path = log,
      .append_output = true_v,
      .timeout_ms = 30000u,
      .termination_grace_ms = 100u,
      .hidden = true_v,
  };
  int32_t exit_code = -1;
  bool8_t timed_out = false_v;
  const bool8_t ran = vkr_platform_process_run(&config, &exit_code, &timed_out);
  uint8_t *data = NULL;
  uint64_t length = 0u;
  const bool8_t read = ran && exit_code == 0 &&
                       vkr_bakery_read_file(log, KB(64), &data, &length);
  (void)vkr_bakery_remove_file(log);
  if (!read) {
    free(data);
    return false_v;
  }
  char *line = (char *)data;
  while (*line == '\n' || *line == '\r' || *line == ' ') {
    line += 1;
  }
  char *end = strpbrk(line, "\r\n");
  if (end) {
    *end = 0;
  }
  (void)snprintf(out, capacity, "%s", line);
  free(data);
  return out[0] != 0;
}

bool8_t vkr_bakery_shader_toolchain(VkrBakeryShaderToolchain *toolchain,
                                    const char *slangc, const char *scratch) {
  MemZero(toolchain, sizeof(*toolchain));
  (void)snprintf(toolchain->slangc, sizeof(toolchain->slangc), "%s",
                 slangc && slangc[0] ? slangc : "");
  if (!toolchain->slangc[0]) {
    const char *environment = getenv("VKR_SLANGC");
    const char *sdk = getenv("VULKAN_SDK");
    if (environment && environment[0]) {
      (void)snprintf(toolchain->slangc, sizeof(toolchain->slangc), "%s",
                     environment);
    } else if (sdk && sdk[0]) {
      (void)snprintf(toolchain->slangc, sizeof(toolchain->slangc),
                     "%s/bin/slangc", sdk);
    }
#if defined(VKR_BAKERY_SLANGC_DEFAULT)
    if (!toolchain->slangc[0] || !vkr_bakery_is_file(toolchain->slangc)) {
      (void)snprintf(toolchain->slangc, sizeof(toolchain->slangc), "%s",
                     VKR_BAKERY_SLANGC_DEFAULT);
    }
#endif
    if (!toolchain->slangc[0]) {
      (void)snprintf(toolchain->slangc, sizeof(toolchain->slangc), "slangc");
    }
  }
  const char *version_argument[] = {"-v"};
  toolchain->has_slangc = vkr_bakery_capture_version(
      scratch, toolchain->slangc, version_argument, 1u,
      toolchain->slangc_version, sizeof(toolchain->slangc_version));
#if defined(__APPLE__)
  const char *metal_arguments[] = {"-sdk", "macosx", "metal", "--version"};
  toolchain->has_metal = vkr_bakery_capture_version(
      scratch, "xcrun", metal_arguments, ArrayCount(metal_arguments),
      toolchain->metal_version, sizeof(toolchain->metal_version));
#endif
  return toolchain->has_slangc;
}

// =============================================================================
// Recipes
// =============================================================================

vkr_internal VkrBakeryJson *vkr_bakery_shader_recipe(VkrBakeryGraph *graph,
                                                     const char *relative,
                                                     char *out_path,
                                                     uint32_t capacity) {
  if (!vkr_bakery_path_join(out_path, capacity, graph->config->root,
                            relative)) {
    return NULL;
  }
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!vkr_bakery_read_file(out_path, MB(4), &data, &length)) {
    vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_REC_UNREADABLE, relative, 0u, 0u,
                          "cannot read the shader library recipe; pass --root "
                          "<repository>",
                          NULL);
    graph->plan_failed = true_v;
    return NULL;
  }
  VkrBakeryJsonError error;
  VkrBakeryJson *recipe =
      vkr_bakery_json_parse(graph->arena, data, length, 32u, &error);
  free(data);
  if (!recipe || recipe->type != VKR_BAKERY_JSON_OBJECT) {
    vkr_bakery_event_diag(
        0u, VKR_BAKERY_DIAG_REC_UNREADABLE, relative, recipe ? 0u : error.line,
        recipe ? 0u : error.column,
        recipe ? "a recipe must be a JSON object" : error.message, NULL);
    graph->plan_failed = true_v;
    return NULL;
  }
  return recipe;
}

/* Parses a Makefile depfile, reporting every prerequisite under the root.
 * Toolchain headers are covered by the tool version in the recipe. */
void vkr_bakery_task_read_depfile(VkrBakeryTask *task, const char *path) {
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!vkr_bakery_read_file(path, MB(16), &data, &length)) {
    return;
  }
  /* Compilers name prerequisites by resolved paths; a root reached through a
   * symbolic link is compared in its resolved form too. */
  char resolved_root[VKR_BAKERY_PATH_CAPACITY] = {0};
#if !defined(_WIN32)
  char resolved[PATH_MAX];
  if (realpath(task->config->root, resolved)) {
    (void)snprintf(resolved_root, sizeof(resolved_root), "%s", resolved);
  }
#endif
  char *text = (char *)data;
  char *cursor = strstr(text, ": ");
  if (!cursor) {
    cursor = strchr(text, ':');
    /* Skip a Windows drive colon in the target name. */
    if (cursor && cursor == text + 1) {
      cursor = strchr(cursor + 1, ':');
    }
  }
  if (!cursor) {
    free(data);
    return;
  }
  cursor += 1;
  char token[VKR_BAKERY_PATH_CAPACITY];
  uint32_t token_length = 0u;
  for (;; ++cursor) {
    const char c = *cursor;
    bool8_t boundary =
        c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '\r';
    if (c == '\\' && (cursor[1] == '\n' || cursor[1] == '\r')) {
      boundary = true_v;
    } else if (c == '\\' && (cursor[1] == ' ' || cursor[1] == ':' ||
                             cursor[1] == '\\' || cursor[1] == '#')) {
      /* Makefile escapes. slangc on Windows writes `E\:\\dir\\file`; kept
       * escaped, no prerequisite resolved below the root and an edited
       * include reused the stale product. */
      if (token_length + 1u < sizeof(token)) {
        token[token_length++] = cursor[1];
      }
      cursor += 1;
      continue;
    }
    if (boundary) {
      if (token_length) {
        token[token_length] = 0;
        char relative[VKR_BAKERY_PATH_CAPACITY];
        const bool8_t below =
            vkr_bakery_path_relative(task->config->root, token, relative,
                                     sizeof(relative)) ||
            (resolved_root[0] &&
             vkr_bakery_path_relative(resolved_root, token, relative,
                                      sizeof(relative)));
        if (below && strncmp(relative, "build", 5u) != 0) {
          vkr_bakery_task_discovered(task, token);
        }
        token_length = 0u;
      }
      if (c == 0) {
        break;
      }
      continue;
    }
    if (token_length + 1u < sizeof(token)) {
      token[token_length++] = c;
    }
  }
  free(data);
}

// =============================================================================
// shader_spirv: one Vulkan entry point
// =============================================================================

vkr_internal const char *const vkr_bakery_spirv_fields[] = {
    "name",      "entry",           "stage",          "profile",
    "arguments", "entry_arguments", "slangc_version", NULL};

vkr_internal bool8_t vkr_bakery_spirv_plan(VkrBakeryGraph *graph,
                                           VkrBakeryAction *action) {
  if (!action->source ||
      !vkr_bakery_action_input(graph, action, action->source,
                               vkr_bakery_path_name(action->source))) {
    return false_v;
  }
  vkr_bakery_action_label(
      graph, action, "%s %s",
      vkr_bakery_recipe_string(action->recipe, "stage", ""),
      vkr_bakery_recipe_string(action->recipe, "entry", ""));
  action->display = vkr_bakery_graph_printf(
      graph, "vulkan/%s", vkr_bakery_recipe_string(action->recipe, "name", ""));
  return true_v;
}

vkr_internal uint32_t vkr_bakery_append_strings(const VkrBakeryJson *array,
                                                const char **arguments,
                                                uint32_t count,
                                                uint32_t capacity) {
  for (const VkrBakeryJson *item = array ? array->first : NULL;
       item && count < capacity; item = item->next) {
    if (item->type == VKR_BAKERY_JSON_STRING) {
      arguments[count++] = (const char *)item->string.str;
    }
  }
  return count;
}

vkr_internal bool8_t vkr_bakery_spirv_run(VkrBakeryTask *task) {
  const VkrBakeryAction *action = task->action;
  const VkrBakeryJson *recipe = action->recipe;
  const char *staged = vkr_bakery_task_stage(task, "entry.spv");
  const char *depfile = vkr_bakery_task_stage(task, "entry.d");
  const char *arguments[56];
  uint32_t count = 0u;
  /* Same argument order as the former CMake rule. */
  arguments[count++] = "-target";
  arguments[count++] = "spirv";
  arguments[count++] = "-profile";
  arguments[count++] = vkr_bakery_recipe_string(recipe, "profile", "");
  count = vkr_bakery_append_strings(vkr_bakery_json_get(recipe, "arguments"),
                                    arguments, count, 44u);
  count = vkr_bakery_append_strings(
      vkr_bakery_json_get(recipe, "entry_arguments"), arguments, count, 44u);
  arguments[count++] = "-entry";
  arguments[count++] = vkr_bakery_recipe_string(recipe, "entry", "");
  arguments[count++] = "-stage";
  arguments[count++] = vkr_bakery_recipe_string(recipe, "stage", "");
  arguments[count++] = action->source;
  arguments[count++] = "-o";
  arguments[count++] = staged;
  arguments[count++] = "-depfile";
  arguments[count++] = depfile;
  int32_t exit_code = -1;
  if (!vkr_bakery_task_process(task, task->config->slangc, arguments, count,
                               &exit_code)) {
    return false_v;
  }
  (void)vkr_bakery_task_parse_compiler_diags(task, VKR_BAKERY_DIAG_SHD_ERROR,
                                             VKR_BAKERY_DIAG_SHD_WARNING);
  if (exit_code != 0) {
    vkr_bakery_task_fail(task, "slangc exited with %d", exit_code);
    return false_v;
  }
  vkr_bakery_task_read_depfile(task, depfile);
  return vkr_bakery_task_product(task, "spv", staged);
}

/* Version 2 discards Windows records whose escaped depfiles named no
 * includes, so their keys ignored every edited include. */
const VkrBakeryProducer vkr_bakery_producer_shader_spirv = {
    .id = "shader_spirv",
    .version = 2u,
    .identity = VKR_BAKERY_IDENTITY_SHADER,
    .summary = "One Vulkan Slang entry point to SPIR-V.",
    .recipe_fields = vkr_bakery_spirv_fields,
    .plan = vkr_bakery_spirv_plan,
    .run = vkr_bakery_spirv_run,
};

// =============================================================================
// shader_msl: the Metal Slang support library
// =============================================================================

vkr_internal const char *const vkr_bakery_msl_fields[] = {
    "name", "slangc_version", NULL};

vkr_internal bool8_t vkr_bakery_msl_plan(VkrBakeryGraph *graph,
                                         VkrBakeryAction *action) {
  action->display = vkr_bakery_graph_printf(
      graph, "metal/%s", vkr_bakery_recipe_string(action->recipe, "name", ""));
  vkr_bakery_action_label(graph, action, "Slang to MSL");
  return action->source &&
         vkr_bakery_action_input(graph, action, action->source,
                                 vkr_bakery_path_name(action->source));
}

vkr_internal bool8_t vkr_bakery_msl_run(VkrBakeryTask *task) {
  const VkrBakeryAction *action = task->action;
  const char *staged = vkr_bakery_task_stage(task, "library.metal");
  const char *depfile = vkr_bakery_task_stage(task, "library.d");
  const char *arguments[] = {"-target", "metal",        "-line-directive-mode",
                             "none",    action->source, "-o",
                             staged,    "-depfile",     depfile};
  int32_t exit_code = -1;
  if (!vkr_bakery_task_process(task, task->config->slangc, arguments,
                               ArrayCount(arguments), &exit_code)) {
    return false_v;
  }
  (void)vkr_bakery_task_parse_compiler_diags(task, VKR_BAKERY_DIAG_SHD_ERROR,
                                             VKR_BAKERY_DIAG_SHD_WARNING);
  if (exit_code != 0) {
    vkr_bakery_task_fail(task, "slangc exited with %d", exit_code);
    return false_v;
  }
  vkr_bakery_task_read_depfile(task, depfile);
  return vkr_bakery_task_product(task, "msl", staged);
}

const VkrBakeryProducer vkr_bakery_producer_shader_msl = {
    .id = "shader_msl",
    .version = 1u,
    .identity = VKR_BAKERY_IDENTITY_SHADER,
    .summary = "The Metal Slang support library to MSL.",
    .recipe_fields = vkr_bakery_msl_fields,
    .plan = vkr_bakery_msl_plan,
    .run = vkr_bakery_msl_run,
};

// =============================================================================
// shader_concat: the native MSL library in declaration order
// =============================================================================

vkr_internal const char *const vkr_bakery_concat_fields[] = {"name", "sources",
                                                             NULL};

vkr_internal bool8_t vkr_bakery_concat_plan(VkrBakeryGraph *graph,
                                            VkrBakeryAction *action) {
  char directory[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_path_parent(directory, sizeof(directory), action->source);
  const VkrBakeryJson *sources = vkr_bakery_json_get(action->recipe, "sources");
  uint32_t index = 0u;
  for (const VkrBakeryJson *item = sources ? sources->first : NULL; item;
       item = item->next, ++index) {
    char path[VKR_BAKERY_PATH_CAPACITY];
    if (item->type != VKR_BAKERY_JSON_STRING ||
        !vkr_bakery_path_join(path, sizeof(path), directory,
                              (const char *)item->string.str) ||
        !vkr_bakery_is_file(path)) {
      vkr_bakery_plan_diag(graph, action, VKR_BAKERY_DIAG_IDX_MISSING_SOURCE,
                           "MSL source %u is missing", index);
      return false_v;
    }
    /* The index prefix keeps declaration order in the sorted key. */
    if (!vkr_bakery_action_input(
            graph, action, path,
            vkr_bakery_graph_printf(graph, "%03u:%s", index,
                                    (const char *)item->string.str))) {
      return false_v;
    }
  }
  action->display = vkr_bakery_graph_printf(
      graph, "metal/%s", vkr_bakery_recipe_string(action->recipe, "name", ""));
  vkr_bakery_action_label(graph, action, "%u native sources", index);
  return index > 0u;
}

vkr_internal int vkr_bakery_compare_names(const void *lhs, const void *rhs) {
  const VkrBakeryInput *a = *(const VkrBakeryInput *const *)lhs;
  const VkrBakeryInput *b = *(const VkrBakeryInput *const *)rhs;
  return strcmp(a->name, b->name);
}

vkr_internal bool8_t vkr_bakery_concat_run(VkrBakeryTask *task) {
  const VkrBakeryAction *action = task->action;
  const VkrBakeryInput **ordered = (const VkrBakeryInput **)malloc(
      sizeof(VkrBakeryInput *) *
      (action->input_count ? action->input_count : 1u));
  if (!ordered) {
    return false_v;
  }
  for (uint32_t i = 0u; i < action->input_count; ++i) {
    ordered[i] = &action->inputs[i];
  }
  qsort(ordered, action->input_count, sizeof(*ordered),
        vkr_bakery_compare_names);
  /* Matches cmake/vkr_concat_shader_sources.cmake: each file then "\n". */
  VkrBakeryBuffer output = {0};
  bool8_t ok = true_v;
  for (uint32_t i = 0u; i < action->input_count && ok; ++i) {
    uint8_t *data = NULL;
    uint64_t length = 0u;
    ok = vkr_bakery_read_file(ordered[i]->path, MB(64), &data, &length);
    vkr_bakery_buffer_append(&output, data, length);
    vkr_bakery_buffer_append(&output, "\n", 1u);
    free(data);
  }
  free(ordered);
  const char *staged = vkr_bakery_task_stage(task, "library.metal");
  ok = ok && !output.failed &&
       vkr_bakery_write_file_atomic(staged, output.data, output.length);
  vkr_bakery_buffer_free(&output);
  if (!ok) {
    vkr_bakery_task_fail(task, "could not concatenate the MSL sources");
    return false_v;
  }
  return vkr_bakery_task_product(task, "msl", staged);
}

const VkrBakeryProducer vkr_bakery_producer_shader_concat = {
    .id = "shader_concat",
    .version = 1u,
    .identity = VKR_BAKERY_IDENTITY_SHADER,
    .summary = "Ordered concatenation of the native Metal shader library.",
    .recipe_fields = vkr_bakery_concat_fields,
    .plan = vkr_bakery_concat_plan,
    .run = vkr_bakery_concat_run,
};

// =============================================================================
// shader_metallib: offline-compiled Metal library
// =============================================================================

vkr_internal const char *const vkr_bakery_metallib_fields[] = {
    "name", "metal_version", "arguments", NULL};

vkr_internal bool8_t vkr_bakery_metallib_plan(VkrBakeryGraph *graph,
                                              VkrBakeryAction *action) {
  action->display = vkr_bakery_graph_printf(
      graph, "metal/%s.metallib",
      vkr_bakery_recipe_string(action->recipe, "name", ""));
  vkr_bakery_action_label(graph, action, "metal -c + metallib");
  return true_v;
}

vkr_internal bool8_t vkr_bakery_metallib_run(VkrBakeryTask *task) {
  const VkrBakeryAction *action = task->action;
  const char *msl = NULL;
  for (uint32_t i = 0u; i < action->input_count; ++i) {
    if (action->inputs[i].dep_action >= 0) {
      msl = action->inputs[i].path;
    }
  }
  if (!msl) {
    vkr_bakery_task_fail(task, "no MSL input");
    return false_v;
  }
  /* The CAS file has no .metal extension; name the language explicitly. */
  const char *air = vkr_bakery_task_stage(task, "library.air");
  const char *depfile = vkr_bakery_task_stage(task, "library.d");
  const char *staged = vkr_bakery_task_stage(task, "library.metallib");
  const char *compile[40];
  uint32_t count = 0u;
  compile[count++] = "-sdk";
  compile[count++] = "macosx";
  compile[count++] = "metal";
  compile[count++] = "-c";
  compile[count++] = "-x";
  compile[count++] = "metal";
  count = vkr_bakery_append_strings(
      vkr_bakery_json_get(action->recipe, "arguments"), compile, count, 30u);
  compile[count++] = "-MMD";
  compile[count++] = "-dependency-file";
  compile[count++] = depfile;
  compile[count++] = msl;
  compile[count++] = "-o";
  compile[count++] = air;
  int32_t exit_code = -1;
  if (!vkr_bakery_task_process(task, "xcrun", compile, count, &exit_code)) {
    return false_v;
  }
  (void)vkr_bakery_task_parse_compiler_diags(task, VKR_BAKERY_DIAG_SHD_ERROR,
                                             VKR_BAKERY_DIAG_SHD_WARNING);
  if (exit_code != 0) {
    vkr_bakery_task_fail(task, "metal exited with %d", exit_code);
    return false_v;
  }
  const char *link[] = {"-sdk", "macosx", "metallib", air, "-o", staged};
  if (!vkr_bakery_task_process(task, "xcrun", link, ArrayCount(link),
                               &exit_code)) {
    return false_v;
  }
  if (exit_code != 0) {
    vkr_bakery_task_fail(task, "metallib exited with %d", exit_code);
    return false_v;
  }
  return vkr_bakery_task_product(task, "metallib", staged);
}

const VkrBakeryProducer vkr_bakery_producer_shader_metallib = {
    .id = "shader_metallib",
    .version = 1u,
    .identity = VKR_BAKERY_IDENTITY_SHADER,
    .summary = "MSL to a precompiled .metallib (macOS Metal toolchain).",
    .recipe_fields = vkr_bakery_metallib_fields,
    .plan = vkr_bakery_metallib_plan,
    .run = vkr_bakery_metallib_run,
};

// =============================================================================
// shader_manifest: the catalog the renderer loads
// =============================================================================

vkr_internal const char *const vkr_bakery_manifest_fields[] = {
    "backend", "entries", "libraries", "tool", "profile", "archive", NULL};

vkr_internal bool8_t vkr_bakery_manifest_plan(VkrBakeryGraph *graph,
                                              VkrBakeryAction *action) {
  const char *backend =
      vkr_bakery_recipe_string(action->recipe, "backend", "vulkan");
  action->display =
      vkr_bakery_graph_printf(graph, "%s/shader_manifest.json", backend);
  vkr_bakery_action_label(graph, action, "catalog");
  return true_v;
}

/* Product hash of the dependency input named `name`, or NULL. */
vkr_internal const VkrBakeryInput *
vkr_bakery_manifest_input(const VkrBakeryAction *action, const char *name) {
  for (uint32_t i = 0u; i < action->input_count; ++i) {
    if (strcmp(action->inputs[i].name, name) == 0) {
      return &action->inputs[i];
    }
  }
  return NULL;
}

vkr_internal bool8_t vkr_bakery_manifest_run(VkrBakeryTask *task) {
  const VkrBakeryAction *action = task->action;
  Arena *arena = task->arena;
  const VkrBakeryJson *recipe = action->recipe;
  VkrBakeryJson *manifest = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, manifest, "v", vkr_bakery_json_int(arena, 1));
  const char *backend = vkr_bakery_recipe_string(recipe, "backend", "vulkan");
  vkr_bakery_json_set(arena, manifest, "backend",
                      vkr_bakery_json_cstr(arena, backend));
  vkr_bakery_json_set(arena, manifest, "tool",
                      vkr_bakery_json_cstr(
                          arena, vkr_bakery_recipe_string(recipe, "tool", "")));
  if (strcmp(backend, "vulkan") == 0) {
    vkr_bakery_json_set(
        arena, manifest, "profile",
        vkr_bakery_json_cstr(arena,
                             vkr_bakery_recipe_string(recipe, "profile", "")));
    VkrBakeryJson *entries = vkr_bakery_json_array(arena);
    const VkrBakeryJson *planned = vkr_bakery_json_get(recipe, "entries");
    for (const VkrBakeryJson *item = planned ? planned->first : NULL; item;
         item = item->next) {
      const char *name = vkr_bakery_recipe_string(item, "name", "");
      const VkrBakeryInput *input = vkr_bakery_manifest_input(action, name);
      if (!input) {
        vkr_bakery_task_fail(task, "entry %s has no product", name);
        return false_v;
      }
      VkrBakeryJson *entry = vkr_bakery_json_object(arena);
      vkr_bakery_json_set(arena, entry, "name",
                          vkr_bakery_json_cstr(arena, name));
      vkr_bakery_json_set(arena, entry, "entry",
                          vkr_bakery_json_cstr(arena, vkr_bakery_recipe_string(
                                                          item, "entry", "")));
      vkr_bakery_json_set(arena, entry, "stage",
                          vkr_bakery_json_cstr(arena, vkr_bakery_recipe_string(
                                                          item, "stage", "")));
      vkr_bakery_json_set(arena, entry, "file",
                          vkr_bakery_json_cstr(arena, vkr_bakery_recipe_string(
                                                          item, "file", "")));
      vkr_bakery_json_set(arena, entry, "hash",
                          vkr_bakery_json_cstr(arena, input->hash));
      vkr_bakery_json_set(arena, entry, "bytes",
                          vkr_bakery_json_int(arena, (int64_t)input->size));
      vkr_bakery_json_append(entries, entry);
    }
    vkr_bakery_json_set(arena, manifest, "entries", entries);
  } else {
    VkrBakeryJson *libraries = vkr_bakery_json_array(arena);
    const VkrBakeryJson *planned = vkr_bakery_json_get(recipe, "libraries");
    for (const VkrBakeryJson *item = planned ? planned->first : NULL; item;
         item = item->next) {
      const char *name = vkr_bakery_recipe_string(item, "name", "");
      char key[160];
      (void)snprintf(key, sizeof(key), "%s.msl", name);
      const VkrBakeryInput *source = vkr_bakery_manifest_input(action, key);
      (void)snprintf(key, sizeof(key), "%s.metallib", name);
      const VkrBakeryInput *metallib = vkr_bakery_manifest_input(action, key);
      if (!source) {
        vkr_bakery_task_fail(task, "library %s has no source", name);
        return false_v;
      }
      VkrBakeryJson *library = vkr_bakery_json_object(arena);
      vkr_bakery_json_set(arena, library, "name",
                          vkr_bakery_json_cstr(arena, name));
      vkr_bakery_json_set(arena, library, "source",
                          vkr_bakery_json_cstr(arena, vkr_bakery_recipe_string(
                                                          item, "source", "")));
      vkr_bakery_json_set(arena, library, "source_hash",
                          vkr_bakery_json_cstr(arena, source->hash));
      if (metallib) {
        vkr_bakery_json_set(
            arena, library, "metallib",
            vkr_bakery_json_cstr(
                arena, vkr_bakery_recipe_string(item, "metallib", "")));
        vkr_bakery_json_set(arena, library, "metallib_hash",
                            vkr_bakery_json_cstr(arena, metallib->hash));
      } else {
        vkr_bakery_json_set(arena, library, "metallib",
                            vkr_bakery_json_null(arena));
      }
      vkr_bakery_json_append(libraries, library);
    }
    vkr_bakery_json_set(arena, manifest, "libraries", libraries);
    vkr_bakery_json_set(
        arena, manifest, "archive",
        vkr_bakery_json_cstr(arena,
                             vkr_bakery_recipe_string(recipe, "archive", "")));
  }
  String8 text = {0};
  const char *staged = vkr_bakery_task_stage(task, "shader_manifest.json");
  if (!vkr_bakery_json_write(arena, manifest, VKR_BAKERY_JSON_PRETTY, &text)) {
    return false_v;
  }
  VkrBakeryBuffer file = {0};
  vkr_bakery_buffer_append(&file, text.str, text.length);
  vkr_bakery_buffer_append(&file, "\n", 1u);
  const bool8_t ok = !file.failed && vkr_bakery_write_file_atomic(
                                         staged, file.data, file.length);
  vkr_bakery_buffer_free(&file);
  return ok && vkr_bakery_task_product(task, "manifest", staged);
}

const VkrBakeryProducer vkr_bakery_producer_shader_manifest = {
    .id = "shader_manifest",
    .version = 1u,
    .identity = VKR_BAKERY_IDENTITY_SHADER,
    .summary = "Per-backend shader catalog loaded by the renderer.",
    .recipe_fields = vkr_bakery_manifest_fields,
    .plan = vkr_bakery_manifest_plan,
    .run = vkr_bakery_manifest_run,
};

// =============================================================================
// Planning
// =============================================================================

vkr_internal bool8_t vkr_bakery_entry_selected(
    const VkrBakeryShaderRequest *request, const char *name) {
  if (request->entry_count == 0u) {
    return true_v;
  }
  for (uint32_t i = 0u; i < request->entry_count; ++i) {
    if (strcmp(request->entries[i], name) == 0) {
      return true_v;
    }
  }
  return false_v;
}

vkr_internal bool8_t vkr_bakery_plan_vulkan(
    VkrBakeryGraph *graph, const VkrBakeryShaderRequest *request,
    const VkrBakeryShaderToolchain *toolchain) {
  Arena *arena = graph->arena;
  char recipe_path[VKR_BAKERY_PATH_CAPACITY];
  VkrBakeryJson *library = vkr_bakery_shader_recipe(
      graph, VKR_BAKERY_VULKAN_RECIPE, recipe_path, sizeof(recipe_path));
  if (!library) {
    return false_v;
  }
  char directory[VKR_BAKERY_PATH_CAPACITY];
  char source[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_path_parent(directory, sizeof(directory), recipe_path);
  if (!vkr_bakery_path_join(
          source, sizeof(source), directory,
          vkr_bakery_recipe_string(library, "library", "library.slang"))) {
    return false_v;
  }
  const VkrBakeryJson *entries = vkr_bakery_json_get(library, "entries");
  VkrBakeryJson *manifest_recipe = vkr_bakery_json_object(arena);
  VkrBakeryJson *manifest_entries = vkr_bakery_json_array(arena);
  VkrBakeryAction **entry_actions = NULL;
  uint32_t selected = 0u;
  uint32_t total = entries ? entries->count : 0u;
  if (total) {
    entry_actions = (VkrBakeryAction **)arena_alloc(
        arena, sizeof(VkrBakeryAction *) * total, ARENA_MEMORY_TAG_ARRAY);
  }
  uint32_t index = 0u;
  for (const VkrBakeryJson *item = entries ? entries->first : NULL; item;
       item = item->next, ++index) {
    const char *name = vkr_bakery_recipe_string(item, "name", NULL);
    if (!name) {
      continue;
    }
    entry_actions[index] = NULL;
    VkrBakeryJson *manifest_entry = vkr_bakery_json_clone(arena, item);
    vkr_bakery_json_remove(manifest_entry, "arguments");
    vkr_bakery_json_set(
        arena, manifest_entry, "file",
        vkr_bakery_json_cstr(arena,
                             vkr_bakery_graph_printf(graph, "%s.spv", name)));
    vkr_bakery_json_append(manifest_entries, manifest_entry);
    if (!vkr_bakery_entry_selected(request, name)) {
      continue;
    }
    VkrBakeryJson *recipe = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, recipe, "name",
                        vkr_bakery_json_cstr(arena, name));
    vkr_bakery_json_set(
        arena, recipe, "entry",
        vkr_bakery_json_clone(arena, vkr_bakery_json_get(item, "entry")));
    vkr_bakery_json_set(
        arena, recipe, "stage",
        vkr_bakery_json_clone(arena, vkr_bakery_json_get(item, "stage")));
    vkr_bakery_json_set(
        arena, recipe, "profile",
        vkr_bakery_json_clone(arena, vkr_bakery_json_get(library, "profile")));
    vkr_bakery_json_set(arena, recipe, "arguments",
                        vkr_bakery_json_clone(
                            arena, vkr_bakery_json_get(library, "arguments")));
    const VkrBakeryJson *extra = vkr_bakery_json_get(item, "arguments");
    if (extra) {
      vkr_bakery_json_set(arena, recipe, "entry_arguments",
                          vkr_bakery_json_clone(arena, extra));
    }
    vkr_bakery_json_set(arena, recipe, "slangc_version",
                        vkr_bakery_json_cstr(arena, toolchain->slangc_version));
    char output[VKR_BAKERY_PATH_CAPACITY];
    (void)snprintf(output, sizeof(output), "%s/vulkan/%s.spv",
                   request->output_directory, name);
    VkrBakeryAction *action = vkr_bakery_graph_add(
        graph, &vkr_bakery_producer_shader_spirv, source, recipe, NULL);
    if (!action || !vkr_bakery_action_output(graph, action, "spv", output)) {
      return false_v;
    }
    action->priority = request->priority;
    entry_actions[index] = action;
    selected += 1u;
  }
  if (request->entry_count && selected != request->entry_count) {
    for (uint32_t i = 0u; i < request->entry_count; ++i) {
      bool8_t known = false_v;
      for (const VkrBakeryJson *item = entries ? entries->first : NULL; item;
           item = item->next) {
        known = known ||
                vkr_bakery_json_is_string(vkr_bakery_json_get(item, "name"),
                                          request->entries[i]);
      }
      if (!known) {
        vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_SHD_UNKNOWN_ENTRY,
                              VKR_BAKERY_VULKAN_RECIPE, 0u, 0u,
                              request->entries[i], NULL);
        graph->plan_failed = true_v;
      }
    }
  }
  /* A partial compile leaves the published catalog as it was. */
  if (request->entry_count) {
    return !graph->plan_failed;
  }
  vkr_bakery_json_set(arena, manifest_recipe, "backend",
                      vkr_bakery_json_cstr(arena, "vulkan"));
  vkr_bakery_json_set(arena, manifest_recipe, "tool",
                      vkr_bakery_json_cstr(arena, toolchain->slangc_version));
  vkr_bakery_json_set(
      arena, manifest_recipe, "profile",
      vkr_bakery_json_clone(arena, vkr_bakery_json_get(library, "profile")));
  vkr_bakery_json_set(arena, manifest_recipe, "entries", manifest_entries);
  VkrBakeryAction *manifest = vkr_bakery_graph_add(
      graph, &vkr_bakery_producer_shader_manifest, NULL, manifest_recipe, NULL);
  if (!manifest) {
    return false_v;
  }
  index = 0u;
  for (const VkrBakeryJson *item = entries ? entries->first : NULL; item;
       item = item->next, ++index) {
    if (entry_actions[index] &&
        !vkr_bakery_action_dep_input(
            graph, manifest, entry_actions[index], "spv",
            vkr_bakery_recipe_string(item, "name", ""))) {
      return false_v;
    }
  }
  char output[VKR_BAKERY_PATH_CAPACITY];
  (void)snprintf(output, sizeof(output), "%s/vulkan/shader_manifest.json",
                 request->output_directory);
  manifest->priority = request->priority;
  return vkr_bakery_action_output(graph, manifest, "manifest", output);
}

vkr_internal bool8_t vkr_bakery_plan_metal(
    VkrBakeryGraph *graph, const VkrBakeryShaderRequest *request,
    const VkrBakeryShaderToolchain *toolchain) {
  Arena *arena = graph->arena;
  char recipe_path[VKR_BAKERY_PATH_CAPACITY];
  VkrBakeryJson *library = vkr_bakery_shader_recipe(
      graph, VKR_BAKERY_METAL_RECIPE, recipe_path, sizeof(recipe_path));
  if (!library) {
    return false_v;
  }
  char directory[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_path_parent(directory, sizeof(directory), recipe_path);
  char slang_source[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bakery_path_join(slang_source, sizeof(slang_source), directory,
                            vkr_bakery_recipe_string(library, "slang_library",
                                                     "slang/library.slang"))) {
    return false_v;
  }
  const char *slang_name =
      vkr_bakery_recipe_string(library, "slang_output", "library.slang.metal");
  const char *source_name =
      vkr_bakery_recipe_string(library, "source_output", "library.metal");

  VkrBakeryJson *msl_recipe = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, msl_recipe, "name",
                      vkr_bakery_json_cstr(arena, slang_name));
  vkr_bakery_json_set(arena, msl_recipe, "slangc_version",
                      vkr_bakery_json_cstr(arena, toolchain->slangc_version));
  VkrBakeryAction *msl = vkr_bakery_graph_add(
      graph, &vkr_bakery_producer_shader_msl, slang_source, msl_recipe, NULL);

  VkrBakeryJson *concat_recipe = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, concat_recipe, "name",
                      vkr_bakery_json_cstr(arena, source_name));
  vkr_bakery_json_set(
      arena, concat_recipe, "sources",
      vkr_bakery_json_clone(arena, vkr_bakery_json_get(library, "sources")));
  VkrBakeryAction *concat =
      vkr_bakery_graph_add(graph, &vkr_bakery_producer_shader_concat,
                           recipe_path, concat_recipe, NULL);
  if (!msl || !concat) {
    return false_v;
  }
  char output[VKR_BAKERY_PATH_CAPACITY];
  (void)snprintf(output, sizeof(output), "%s/metal/%s",
                 request->output_directory, slang_name);
  if (!vkr_bakery_action_output(graph, msl, "msl", output)) {
    return false_v;
  }
  (void)snprintf(output, sizeof(output), "%s/metal/%s",
                 request->output_directory, source_name);
  if (!vkr_bakery_action_output(graph, concat, "msl", output)) {
    return false_v;
  }
  msl->priority = request->priority;
  concat->priority = request->priority;

  if (!toolchain->has_metal) {
    vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_SHD_TOOLCHAIN_MISSING, NULL, 0u,
                          0u, NULL, NULL);
  }
  VkrBakeryJson *manifest_recipe = vkr_bakery_json_object(arena);
  VkrBakeryJson *manifest_libraries = vkr_bakery_json_array(arena);
  VkrBakeryAction *libraries[2] = {msl, concat};
  const char *names[2] = {slang_name, source_name};
  VkrBakeryAction *metallibs[2] = {NULL, NULL};
  for (uint32_t i = 0u; i < 2u; ++i) {
    char stem[256];
    (void)snprintf(stem, sizeof(stem), "%s", names[i]);
    char *extension = strrchr(stem, '.');
    if (extension && strcmp(extension, ".metal") == 0) {
      *extension = 0;
    }
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, entry, "name",
                        vkr_bakery_json_cstr(arena, stem));
    vkr_bakery_json_set(arena, entry, "source",
                        vkr_bakery_json_cstr(arena, names[i]));
    const char *metallib_name =
        vkr_bakery_graph_printf(graph, "%s.metallib", stem);
    vkr_bakery_json_set(arena, entry, "metallib",
                        vkr_bakery_json_cstr(arena, metallib_name));
    vkr_bakery_json_append(manifest_libraries, entry);
    if (!toolchain->has_metal) {
      continue;
    }
    VkrBakeryJson *recipe = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, recipe, "name",
                        vkr_bakery_json_cstr(arena, stem));
    vkr_bakery_json_set(arena, recipe, "metal_version",
                        vkr_bakery_json_cstr(arena, toolchain->metal_version));
    /* The runtime source compiler implicitly includes metal_stdlib; the
     * offline compile must see the same prelude. */
    VkrBakeryJson *arguments =
        vkr_bakery_json_get(library, "metal_arguments")
            ? vkr_bakery_json_clone(
                  arena, vkr_bakery_json_get(library, "metal_arguments"))
            : vkr_bakery_json_array(arena);
    if (!arguments->count) {
      vkr_bakery_json_append(arguments,
                             vkr_bakery_json_cstr(arena, "-include"));
      vkr_bakery_json_append(arguments,
                             vkr_bakery_json_cstr(arena, "metal_stdlib"));
    }
    vkr_bakery_json_set(arena, recipe, "arguments", arguments);
    VkrBakeryAction *metallib = vkr_bakery_graph_add(
        graph, &vkr_bakery_producer_shader_metallib, NULL, recipe, NULL);
    if (!metallib || !vkr_bakery_action_dep_input(graph, metallib, libraries[i],
                                                  "msl", "source")) {
      return false_v;
    }
    (void)snprintf(output, sizeof(output), "%s/metal/%s",
                   request->output_directory, metallib_name);
    if (!vkr_bakery_action_output(graph, metallib, "metallib", output)) {
      return false_v;
    }
    metallib->priority = request->priority;
    metallibs[i] = metallib;
  }
  vkr_bakery_json_set(arena, manifest_recipe, "backend",
                      vkr_bakery_json_cstr(arena, "metal"));
  vkr_bakery_json_set(arena, manifest_recipe, "tool",
                      vkr_bakery_json_cstr(arena, toolchain->has_metal
                                                      ? toolchain->metal_version
                                                      : ""));
  vkr_bakery_json_set(arena, manifest_recipe, "libraries", manifest_libraries);
  vkr_bakery_json_set(
      arena, manifest_recipe, "archive",
      vkr_bakery_json_clone(arena, vkr_bakery_json_get(library, "archive")));
  VkrBakeryAction *manifest = vkr_bakery_graph_add(
      graph, &vkr_bakery_producer_shader_manifest, NULL, manifest_recipe, NULL);
  if (!manifest) {
    return false_v;
  }
  for (uint32_t i = 0u; i < 2u; ++i) {
    char stem[256];
    (void)snprintf(stem, sizeof(stem), "%s", names[i]);
    char *extension = strrchr(stem, '.');
    if (extension && strcmp(extension, ".metal") == 0) {
      *extension = 0;
    }
    if (!vkr_bakery_action_dep_input(
            graph, manifest, libraries[i], "msl",
            vkr_bakery_graph_printf(graph, "%s.msl", stem)) ||
        (metallibs[i] &&
         !vkr_bakery_action_dep_input(
             graph, manifest, metallibs[i], "metallib",
             vkr_bakery_graph_printf(graph, "%s.metallib", stem)))) {
      return false_v;
    }
  }
  (void)snprintf(output, sizeof(output), "%s/metal/shader_manifest.json",
                 request->output_directory);
  manifest->priority = request->priority;
  return vkr_bakery_action_output(graph, manifest, "manifest", output);
}

bool8_t vkr_bakery_plan_shaders(VkrBakeryGraph *graph,
                                const VkrBakeryShaderRequest *request) {
  const VkrBakeryShaderToolchain *toolchain = request->toolchain;
  if (!toolchain || !toolchain->has_slangc) {
    vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_SCHED_LAUNCH_FAILED, NULL, 0u, 0u,
                          "slangc was not found; pass --slangc, set VKR_SLANGC "
                          "or VULKAN_SDK",
                          NULL);
    graph->plan_failed = true_v;
    return false_v;
  }
  bool8_t ok = true_v;
  if (request->vulkan) {
    ok = vkr_bakery_plan_vulkan(graph, request, toolchain) && ok;
  }
  if (request->metal && request->entry_count == 0u) {
    ok = vkr_bakery_plan_metal(graph, request, toolchain) && ok;
  }
  return ok && !graph->plan_failed;
}
