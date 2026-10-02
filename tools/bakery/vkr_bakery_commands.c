#include "vkr_bakery_commands.h"

#include "platform/vkr_platform.h"
#include "vkr_bakery_buffer.h"
#include "vkr_bakery_identity.h"
#include "vkr_bakery_shaders.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =============================================================================
// Shared helpers
// =============================================================================

int vkr_bakery_usage(const char *message) {
  vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_CLI_USAGE, NULL, 0u, 0u, message,
                        NULL);
  return VKR_BAKERY_EXIT_USAGE;
}

/* Parses --recipe key=value pairs; values are JSON when they parse as JSON,
 * otherwise strings. */
vkr_internal VkrBakeryJson *vkr_bakery_cli_overrides(VkrBakeryCli *cli,
                                                     Arena *arena) {
  VkrBakeryJson *overrides = vkr_bakery_json_object(arena);
  for (uint32_t i = 0u; i < cli->recipe_count; ++i) {
    const char *pair = cli->recipes[i];
    const char *equals = strchr(pair, '=');
    char key[128];
    (void)snprintf(key, sizeof(key), "%.*s", (int)(equals - pair), pair);
    const char *text = equals + 1;
    VkrBakeryJson *value = vkr_bakery_json_parse(arena, (const uint8_t *)text,
                                                 strlen(text), 32u, NULL);
    if (!value) {
      value = vkr_bakery_json_cstr(arena, text);
    }
    vkr_bakery_json_set(arena, overrides, key, value);
  }
  return overrides;
}

vkr_internal int vkr_bakery_execute(VkrBakeryCli *cli, VkrBakeryGraph *graph) {
  if (graph->plan_failed) {
    return VKR_BAKERY_EXIT_USAGE;
  }
  if (!vkr_bakery_cache_prepare(&cli->config)) {
    vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_CACHE_UNWRITABLE,
                          cli->config.cache_dir, 0u, 0u, NULL, NULL);
    return VKR_BAKERY_EXIT_ENVIRONMENT;
  }
  const bool8_t ok = vkr_bakery_graph_execute(graph);
  if (vkr_atomic_bool_load(&vkr_bakery_cancel_requested,
                           VKR_MEMORY_ORDER_RELAXED)) {
    return VKR_BAKERY_EXIT_CANCELLED;
  }
  return ok ? VKR_BAKERY_EXIT_OK : VKR_BAKERY_EXIT_FAILED;
}

vkr_internal void vkr_bakery_emit_run(VkrBakeryCli *cli) {
  vkr_bakery_event_run(cli->command, cli->config.platform, cli->config.jobs,
                       cli->config.memory_budget_mib, cli->config.cache_dir,
                       VKR_BAKERY_IDENTITY_BUILD);
}

/* Directory walks add every file some producer cooks. Font configurations
 * that are not cooked MTSDF atlases (bitmap or system fonts) are skipped. */
/* `<module>.script.json` describes a C script module (vkr_bakery_script.c). */
vkr_internal bool8_t vkr_bakery_is_script(const char *path) {
  const char *suffix = ".script.json";
  const uint64_t length = strlen(path);
  const uint64_t suffix_length = strlen(suffix);
  return length > suffix_length &&
         strcmp(path + length - suffix_length, suffix) == 0;
}

vkr_internal bool8_t vkr_bakery_is_cookable(const char *path) {
  const char *name = vkr_bakery_path_name(path);
  if (name[0] == '.') {
    return false_v;
  }
  if (vkr_bakery_is_script(path)) {
    return true_v;
  }
  const VkrBakeryProducer *producer = vkr_bakery_producer_for_source(path);
  if (!producer) {
    return false_v;
  }
  if (strcmp(producer->id, "font") == 0) {
    uint8_t *data = NULL;
    uint64_t length = 0u;
    if (!vkr_bakery_read_file(path, MB(1), &data, &length)) {
      return false_v;
    }
    const bool8_t cooked =
        strstr((const char *)data, "type=cooked_mtsdf") != NULL;
    free(data);
    return cooked;
  }
  return true_v;
}

typedef struct VkrBakeryAddContext {
  VkrBakeryCli *cli;
  VkrBakeryGraph *graph;
  VkrBakeryJson *overrides;
  const char *directory;
  bool8_t single;
} VkrBakeryAddContext;

vkr_internal bool8_t vkr_bakery_add_source(VkrBakeryAddContext *context,
                                           const char *source,
                                           const char *producer_id,
                                           const char *output,
                                           const VkrBakeryJson *extra) {
  VkrBakeryGraph *graph = context->graph;
  if (vkr_bakery_is_script(source)) {
    /* A script module plans several actions; --out names their directory. */
    const char *directory = output            ? output
                            : context->single ? context->cli->out
                                              : NULL;
    if (!vkr_bakery_plan_script(graph, source, directory)) {
      return false_v;
    }
    graph->root_count += 1u;
    return true_v;
  }
  VkrBakeryJson *overrides =
      vkr_bakery_json_clone(graph->arena, context->overrides);
  for (const VkrBakeryJson *field = extra ? extra->first : NULL; field;
       field = field->next) {
    vkr_bakery_json_set(graph->arena, overrides, (const char *)field->key.str,
                        vkr_bakery_json_clone(graph->arena, field));
  }
  VkrBakeryJson *recipe =
      vkr_bakery_recipe_load(graph, NULL, source, overrides);
  if (!recipe) {
    return false_v;
  }
  const char *recipe_producer =
      vkr_bakery_recipe_string(recipe, "producer", NULL);
  const char *recipe_output = vkr_bakery_recipe_string(recipe, "output", NULL);
  const char *id = producer_id              ? producer_id
                   : context->cli->producer ? context->cli->producer
                                            : recipe_producer;
  const VkrBakeryProducer *producer =
      id ? vkr_bakery_producer_find(id)
         : vkr_bakery_producer_for_source(source);
  if (!producer) {
    vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_REC_NO_PRODUCER,
                          vkr_bakery_graph_display(graph, source), 0u, 0u,
                          id ? "unknown producer" : NULL, NULL);
    graph->plan_failed = true_v;
    return false_v;
  }
  char resolved_output[VKR_BAKERY_PATH_CAPACITY];
  const char *requested = output;
  if (!requested && context->single && context->cli->out) {
    requested = context->cli->out;
  }
  if (!requested && recipe_output) {
    char directory[VKR_BAKERY_PATH_CAPACITY];
    vkr_bakery_path_parent(directory, sizeof(directory), source);
    if (vkr_bakery_path_is_absolute(recipe_output) ||
        !vkr_bakery_path_join(resolved_output, sizeof(resolved_output),
                              directory, recipe_output)) {
      (void)snprintf(resolved_output, sizeof(resolved_output), "%s",
                     recipe_output);
    }
    requested = resolved_output;
  }
  vkr_bakery_json_remove(recipe, "producer");
  vkr_bakery_json_remove(recipe, "output");
  VkrBakeryAction *action =
      vkr_bakery_graph_add(graph, producer, source, recipe, requested);
  if (action) {
    graph->root_count += 1u;
  }
  return action != NULL;
}

vkr_internal bool8_t vkr_bakery_walk_visit(void *context, const char *name,
                                           bool8_t is_directory) {
  VkrBakeryAddContext *add = (VkrBakeryAddContext *)context;
  char path[VKR_BAKERY_PATH_CAPACITY];
  if (name[0] == '.' ||
      !vkr_bakery_path_join(path, sizeof(path), add->directory, name)) {
    return true_v;
  }
  if (is_directory) {
    VkrBakeryAddContext child = *add;
    child.directory = path;
    return vkr_bakery_list_directory(path, vkr_bakery_walk_visit, &child);
  }
  if (vkr_bakery_is_cookable(path)) {
    (void)vkr_bakery_add_source(add, path, NULL, NULL, NULL);
  }
  return true_v;
}

/* Adds positional sources and directories to the graph. */
vkr_internal void vkr_bakery_add_positionals(VkrBakeryCli *cli,
                                             VkrBakeryGraph *graph,
                                             VkrBakeryJson *overrides) {
  VkrBakeryAddContext context = {
      .cli = cli,
      .graph = graph,
      .overrides = overrides,
      .single = cli->positional_count == 1u,
  };
  for (uint32_t i = 0u; i < cli->positional_count; ++i) {
    const char *path = cli->positional[i];
    if (vkr_bakery_is_directory(path)) {
      context.single = false_v;
      context.directory = path;
      (void)vkr_bakery_list_directory(path, vkr_bakery_walk_visit, &context);
    } else if (vkr_bakery_is_file(path)) {
      (void)vkr_bakery_add_source(&context, path, NULL, NULL, NULL);
    } else {
      vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_IDX_MISSING_SOURCE, path, 0u,
                            0u, NULL, NULL);
      graph->plan_failed = true_v;
    }
  }
}

/* Reads a bakery manifest: {"v":1,"root":"..","targets":[{"source",
 * "producer","output","recipe"}]} with paths relative to its root. */
vkr_internal bool8_t vkr_bakery_add_manifest(VkrBakeryCli *cli,
                                             VkrBakeryGraph *graph,
                                             const char *path,
                                             VkrBakeryJson *overrides) {
  uint8_t *data = NULL;
  uint64_t length = 0u;
  VkrBakeryJsonError error = {0};
  VkrBakeryJson *manifest = NULL;
  if (vkr_bakery_read_file(path, MB(16), &data, &length)) {
    manifest = vkr_bakery_json_parse(graph->arena, data, length, 32u, &error);
    free(data);
  }
  const VkrBakeryJson *targets = vkr_bakery_json_get(manifest, "targets");
  if (!targets || targets->type != VKR_BAKERY_JSON_ARRAY) {
    vkr_bakery_event_diag(
        0u, VKR_BAKERY_DIAG_REC_UNREADABLE, path, error.line, error.column,
        error.message[0] ? error.message : "a manifest needs a targets array",
        NULL);
    graph->plan_failed = true_v;
    return false_v;
  }
  char directory[VKR_BAKERY_PATH_CAPACITY];
  char root[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  const char *manifest_root = vkr_bakery_recipe_string(manifest, "root", ".");
  if (!vkr_bakery_path_join(root, sizeof(root), directory[0] ? directory : ".",
                            manifest_root)) {
    return false_v;
  }
  VkrBakeryAddContext context = {
      .cli = cli, .graph = graph, .overrides = overrides};
  uint32_t index = 0u;
  for (const VkrBakeryJson *target = targets->first; target;
       target = target->next, ++index) {
    const char *source = vkr_bakery_recipe_string(target, "source", NULL);
    const char *producer = vkr_bakery_recipe_string(target, "producer", NULL);
    const char *output = vkr_bakery_recipe_string(target, "output", NULL);
    char source_path[VKR_BAKERY_PATH_CAPACITY];
    char output_path[VKR_BAKERY_PATH_CAPACITY];
    if (source &&
        !vkr_bakery_path_join(source_path, sizeof(source_path), root, source)) {
      continue;
    }
    if (output &&
        !vkr_bakery_path_join(output_path, sizeof(output_path), root, output)) {
      continue;
    }
    const bool8_t optional =
        vkr_bakery_recipe_bool(target, "optional", false_v);
    if (source && optional && !vkr_bakery_is_file(source_path) &&
        !vkr_bakery_is_directory(source_path)) {
      vkr_bakery_event_log(0u, "info", "skipped missing optional source",
                           strlen("skipped missing optional source"));
      continue;
    }
    if (!source) {
      /* Source-less producers such as renderer tables. */
      const VkrBakeryProducer *found =
          producer ? vkr_bakery_producer_find(producer) : NULL;
      if (!found) {
        vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_REC_NO_PRODUCER, path, 0u, 0u,
                              "a target needs a source or a producer", NULL);
        graph->plan_failed = true_v;
        continue;
      }
      VkrBakeryJson *recipe = vkr_bakery_json_clone(
          graph->arena, vkr_bakery_json_get(target, "recipe"));
      if (vkr_bakery_graph_add(graph, found, NULL, recipe,
                               output ? output_path : NULL)) {
        graph->root_count += 1u;
      }
      continue;
    }
    if (vkr_bakery_is_directory(source_path)) {
      /* A directory target cooks every supported file below it. */
      VkrBakeryAddContext walk = context;
      walk.directory = source_path;
      (void)vkr_bakery_list_directory(source_path, vkr_bakery_walk_visit,
                                      &walk);
      continue;
    }
    (void)vkr_bakery_add_source(&context, source_path, producer,
                                output ? output_path : NULL,
                                vkr_bakery_json_get(target, "recipe"));
  }
  return !graph->plan_failed;
}

// =============================================================================
// cook, build
// =============================================================================

int vkr_bakery_cmd_cook(VkrBakeryCli *cli) {
  if (cli->positional_count == 0u && !cli->producer) {
    return vkr_bakery_usage(
        "cook needs a source, a directory, or --producer for a source-less "
        "producer such as table");
  }
  if (cli->out && cli->positional_count > 1u) {
    return vkr_bakery_usage("--out applies to exactly one source");
  }
  vkr_bakery_emit_run(cli);
  VkrBakeryGraph graph;
  if (!vkr_bakery_graph_init(&graph, &cli->config)) {
    return VKR_BAKERY_EXIT_ENVIRONMENT;
  }
  VkrBakeryJson *overrides = vkr_bakery_cli_overrides(cli, graph.arena);
  if (cli->positional_count == 0u) {
    const VkrBakeryProducer *producer = vkr_bakery_producer_find(cli->producer);
    if (!producer) {
      vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_REC_NO_PRODUCER, cli->producer,
                            0u, 0u, "unknown producer", NULL);
      vkr_bakery_graph_shutdown(&graph);
      return VKR_BAKERY_EXIT_USAGE;
    }
    if (vkr_bakery_graph_add(&graph, producer, NULL, overrides, cli->out)) {
      graph.root_count = 1u;
    }
  }
  vkr_bakery_add_positionals(cli, &graph, overrides);
  const int code = vkr_bakery_execute(cli, &graph);
  vkr_bakery_graph_shutdown(&graph);
  return code;
}

int vkr_bakery_cmd_scripts(VkrBakeryCli *cli) {
  if (cli->positional_count != 1u || !cli->name) {
    return vkr_bakery_usage(
        "scripts needs one Scripts folder and --name <project>");
  }
  vkr_bakery_emit_run(cli);
  VkrBakeryGraph graph;
  if (!vkr_bakery_graph_init(&graph, &cli->config)) {
    return VKR_BAKERY_EXIT_ENVIRONMENT;
  }
  if (vkr_bakery_plan_script_project(&graph, cli->positional[0], cli->name,
                                     cli->out)) {
    graph.root_count = 1u;
  }
  const int code = vkr_bakery_execute(cli, &graph);
  vkr_bakery_graph_shutdown(&graph);
  return code;
}

int vkr_bakery_cmd_build(VkrBakeryCli *cli) {
  if (cli->positional_count == 0u) {
    return vkr_bakery_usage("build needs a bakery manifest");
  }
  vkr_bakery_emit_run(cli);
  VkrBakeryGraph graph;
  if (!vkr_bakery_graph_init(&graph, &cli->config)) {
    return VKR_BAKERY_EXIT_ENVIRONMENT;
  }
  VkrBakeryJson *overrides = vkr_bakery_cli_overrides(cli, graph.arena);
  for (uint32_t i = 0u; i < cli->positional_count; ++i) {
    (void)vkr_bakery_add_manifest(cli, &graph, cli->positional[i], overrides);
  }
  const int code = vkr_bakery_execute(cli, &graph);
  vkr_bakery_graph_shutdown(&graph);
  return code;
}

// =============================================================================
// shaders
// =============================================================================

typedef struct VkrBakeryWatchState {
  uint64_t signature;
  uint32_t files;
} VkrBakeryWatchState;

vkr_internal bool8_t vkr_bakery_watch_visit(void *context, const char *name,
                                            bool8_t is_directory);

typedef struct VkrBakeryWatchWalk {
  VkrBakeryWatchState *state;
  const char *directory;
} VkrBakeryWatchWalk;

vkr_internal bool8_t vkr_bakery_watch_visit(void *context, const char *name,
                                            bool8_t is_directory) {
  VkrBakeryWatchWalk *walk = (VkrBakeryWatchWalk *)context;
  char path[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bakery_path_join(path, sizeof(path), walk->directory, name)) {
    return true_v;
  }
  if (is_directory) {
    VkrBakeryWatchWalk child = {.state = walk->state, .directory = path};
    return vkr_bakery_list_directory(path, vkr_bakery_watch_visit, &child);
  }
  VkrBakeryStat info;
  if (vkr_bakery_stat(path, &info) && info.exists) {
    /* Order-sensitive mix of names, sizes and times. */
    uint64_t mix = walk->state->signature ^ (uint64_t)info.mtime_ns;
    mix = mix * 1099511628211ull ^ info.size;
    for (const char *c = name; *c; ++c) {
      mix = (mix ^ (uint8_t)*c) * 1099511628211ull;
    }
    walk->state->signature = mix;
    walk->state->files += 1u;
  }
  return true_v;
}

vkr_internal VkrBakeryWatchState vkr_bakery_shader_signature(const char *root) {
  VkrBakeryWatchState state = {.signature = 1469598103934665603ull};
  char directory[VKR_BAKERY_PATH_CAPACITY];
  (void)vkr_bakery_path_join(directory, sizeof(directory), root,
                             "renderer/src/shaders");
  VkrBakeryWatchWalk walk = {.state = &state, .directory = directory};
  (void)vkr_bakery_list_directory(directory, vkr_bakery_watch_visit, &walk);
  return state;
}

int vkr_bakery_cmd_shaders(VkrBakeryCli *cli) {
  if (!cli->out) {
    return vkr_bakery_usage("shaders needs --out <catalog directory>");
  }
  char output[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bakery_path_absolute(cli->out, output, sizeof(output))) {
    return vkr_bakery_usage("invalid --out path");
  }
  const char *backend = cli->backend ? cli->backend : "all";
  VkrBakeryShaderRequest request = {
      .entries = cli->entries,
      .entry_count = cli->entry_count,
      .output_directory = output,
      .priority = VKR_BAKERY_PRIORITY_INTERACTIVE,
  };
  if (strcmp(backend, "all") == 0) {
    request.vulkan = true_v;
#if defined(__APPLE__)
    request.metal = true_v;
#endif
  } else if (strcmp(backend, "vulkan") == 0) {
    request.vulkan = true_v;
  } else if (strcmp(backend, "metal") == 0) {
    request.metal = true_v;
  } else {
    return vkr_bakery_usage("--backend must be vulkan, metal or all");
  }
  if (!vkr_bakery_cache_prepare(&cli->config)) {
    vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_CACHE_UNWRITABLE,
                          cli->config.cache_dir, 0u, 0u, NULL, NULL);
    return VKR_BAKERY_EXIT_ENVIRONMENT;
  }
  char scratch[VKR_BAKERY_PATH_CAPACITY];
  (void)vkr_bakery_path_join(scratch, sizeof(scratch), cli->config.cache_dir,
                             "tmp");
  VkrBakeryShaderToolchain toolchain;
  (void)vkr_bakery_shader_toolchain(&toolchain, cli->config.slangc, scratch);
  (void)snprintf(cli->config.slangc, sizeof(cli->config.slangc), "%s",
                 toolchain.slangc);
  request.toolchain = &toolchain;
  if (!toolchain.has_slangc) {
    vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_SCHED_LAUNCH_FAILED,
                          toolchain.slangc, 0u, 0u,
                          "slangc was not found; pass --slangc, set VKR_SLANGC "
                          "or VULKAN_SDK",
                          NULL);
    return VKR_BAKERY_EXIT_ENVIRONMENT;
  }
  vkr_bakery_emit_run(cli);
  int code = VKR_BAKERY_EXIT_OK;
  VkrBakeryWatchState last = {0};
  for (;;) {
    if (cli->watch) {
      last = vkr_bakery_shader_signature(cli->config.root);
    }
    VkrBakeryGraph graph;
    if (!vkr_bakery_graph_init(&graph, &cli->config)) {
      return VKR_BAKERY_EXIT_ENVIRONMENT;
    }
    (void)vkr_bakery_plan_shaders(&graph, &request);
    code = vkr_bakery_execute(cli, &graph);
    vkr_bakery_graph_shutdown(&graph);
    if (!cli->watch || code == VKR_BAKERY_EXIT_CANCELLED ||
        code == VKR_BAKERY_EXIT_USAGE || code == VKR_BAKERY_EXIT_ENVIRONMENT) {
      break;
    }
    vkr_bakery_print("watching %s/renderer/src/shaders (Ctrl-C to stop)\n",
                     cli->config.root);
    for (;;) {
      if (vkr_atomic_bool_load(&vkr_bakery_cancel_requested,
                               VKR_MEMORY_ORDER_RELAXED)) {
        return VKR_BAKERY_EXIT_OK;
      }
      vkr_platform_sleep(250u);
      const VkrBakeryWatchState now =
          vkr_bakery_shader_signature(cli->config.root);
      if (now.signature != last.signature || now.files != last.files) {
        break;
      }
    }
  }
  return code;
}

// =============================================================================
// inspect, explain, status
// =============================================================================

int vkr_bakery_run_tool_inline(const char *tool, const char *const *arguments,
                               uint32_t count) {
  char *argv[16];
  argv[0] = (char *)"tool";
  argv[1] = (char *)tool;
  for (uint32_t i = 0u; i < count && i + 2u < ArrayCount(argv); ++i) {
    argv[i + 2u] = (char *)arguments[i];
  }
  return vkr_bakery_tool_main((int)count + 2, argv);
}

vkr_internal void vkr_bakery_print_json(Arena *arena,
                                        const VkrBakeryJson *value) {
  String8 text = {0};
  if (vkr_bakery_json_write(arena, value, VKR_BAKERY_JSON_PRETTY, &text)) {
    vkr_bakery_print("%.*s\n", (int)text.length, text.str);
  }
}

vkr_internal const char *const vkr_bakery_probe_names[] = {
    "fresh", "output-stale", "stale", "pending", "error"};

int vkr_bakery_cmd_inspect(VkrBakeryCli *cli) {
  if (cli->positional_count != 1u) {
    return vkr_bakery_usage("inspect needs one artifact or source");
  }
  const char *path = cli->positional[0];
  char extension[16];
  vkr_bakery_path_extension(path, extension, sizeof(extension));
  if (strcmp(extension, ".vkb") == 0) {
    char report[VKR_BAKERY_PATH_CAPACITY];
    vkr_bakery_temp_path(report, sizeof(report), cli->config.cache_dir,
                         "inspect.json");
    (void)vkr_bakery_make_directories(cli->config.cache_dir);
    const char *arguments[] = {"--inspect", "--input", path, "--output",
                               report};
    const int code =
        vkr_bakery_run_tool_inline("mesh", arguments, ArrayCount(arguments));
    uint8_t *data = NULL;
    uint64_t length = 0u;
    if (code == 0 && vkr_bakery_read_file(report, MB(64), &data, &length)) {
      VkrBakeryJson *value = vkr_bakery_json_parse((Arena *)cli->allocator.ctx,
                                                   data, length, 64u, NULL);
      if (value) {
        vkr_bakery_print_json((Arena *)cli->allocator.ctx, value);
      }
      free(data);
    }
    (void)vkr_bakery_remove_file(report);
    return code ? VKR_BAKERY_EXIT_FAILED : VKR_BAKERY_EXIT_OK;
  }
  if (strcmp(extension, ".vka") == 0 || strcmp(extension, ".vkc") == 0) {
    const char *arguments[] = {"--inspect", "--input", path};
    return vkr_bakery_run_tool_inline(
               strcmp(extension, ".vka") == 0 ? "animation" : "collision",
               arguments, ArrayCount(arguments))
               ? VKR_BAKERY_EXIT_FAILED
               : VKR_BAKERY_EXIT_OK;
  }
  /* A source: show its planned action, key and cache state. */
  VkrBakeryGraph graph;
  if (!vkr_bakery_graph_init(&graph, &cli->config)) {
    return VKR_BAKERY_EXIT_ENVIRONMENT;
  }
  int code = VKR_BAKERY_EXIT_OK;
  const VkrBakeryProducer *producer =
      cli->producer ? vkr_bakery_producer_find(cli->producer)
                    : vkr_bakery_producer_for_source(path);
  if (!producer) {
    char hash[VKR_BAKERY_KEY_SIZE];
    uint64_t size = 0u;
    if (!vkr_bakery_hash_file(path, hash, &size)) {
      vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_IDX_MISSING_SOURCE, path, 0u,
                            0u, NULL, NULL);
      code = VKR_BAKERY_EXIT_FAILED;
    } else {
      vkr_bakery_print("%s\n  bytes  %llu\n  sha256 %s\n  (run `vkr_bakery "
                       "explain %s` to find the action that produced it)\n",
                       path, (unsigned long long)size, hash, path);
    }
    vkr_bakery_graph_shutdown(&graph);
    return code;
  }
  VkrBakeryAddContext context = {.cli = cli,
                                 .graph = &graph,
                                 .overrides =
                                     vkr_bakery_cli_overrides(cli, graph.arena),
                                 .single = true_v};
  if (!vkr_bakery_add_source(&context, path, producer->id, NULL, NULL)) {
    vkr_bakery_graph_shutdown(&graph);
    return VKR_BAKERY_EXIT_USAGE;
  }
  VkrBakeryAction *action = graph.actions[0];
  const VkrBakeryProbe probe = vkr_bakery_graph_probe(&graph, action);
  vkr_bakery_print("%s\n  producer  %s v%u (%s)\n  label     %s\n",
                   action->display, producer->id, producer->version,
                   producer->identity, action->label);
  vkr_bakery_print("  recipe\n");
  vkr_bakery_print_json(graph.arena, action->recipe);
  vkr_bakery_print("  inputs\n");
  for (uint32_t i = 0u; i < action->input_count; ++i) {
    vkr_bakery_print("    %.12s  %s\n", action->inputs[i].hash,
                     action->inputs[i].name);
  }
  vkr_bakery_print("  prekey    %s\n  state     %s\n", action->prekey,
                   vkr_bakery_probe_names[probe]);
  for (uint32_t i = 0u; i < action->output_count; ++i) {
    vkr_bakery_print("  output    %s -> %s\n", action->outputs[i].role,
                     vkr_bakery_graph_display(&graph, action->outputs[i].path));
  }
  if (action->key[0]) {
    vkr_bakery_print("  key       %s\n", action->key);
  }
  vkr_bakery_graph_shutdown(&graph);
  return code;
}

typedef struct VkrBakeryFindRecord {
  const VkrBakeryConfig *config;
  const char *hash;
  char found[VKR_BAKERY_PATH_CAPACITY];
  const char *directory;
} VkrBakeryFindRecord;

vkr_internal bool8_t vkr_bakery_find_record_visit(void *context,
                                                  const char *name,
                                                  bool8_t is_directory) {
  VkrBakeryFindRecord *find = (VkrBakeryFindRecord *)context;
  char path[VKR_BAKERY_PATH_CAPACITY];
  if (find->found[0] ||
      !vkr_bakery_path_join(path, sizeof(path), find->directory, name)) {
    return true_v;
  }
  if (is_directory) {
    VkrBakeryFindRecord child = *find;
    child.directory = path;
    (void)vkr_bakery_list_directory(path, vkr_bakery_find_record_visit, &child);
    if (child.found[0]) {
      (void)snprintf(find->found, sizeof(find->found), "%s", child.found);
    }
    return true_v;
  }
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (vkr_bakery_read_file(path, MB(64), &data, &length)) {
    char needle[80];
    (void)snprintf(needle, sizeof(needle), "\"hash\": \"%s\"", find->hash);
    if (strstr((const char *)data, needle)) {
      (void)snprintf(find->found, sizeof(find->found), "%s", path);
    }
    free(data);
  }
  return true_v;
}

int vkr_bakery_cmd_explain(VkrBakeryCli *cli) {
  if (cli->positional_count != 1u) {
    return vkr_bakery_usage("explain needs one artifact or action key");
  }
  const char *target = cli->positional[0];
  char record_path[VKR_BAKERY_PATH_CAPACITY] = {0};
  const uint64_t length = strlen(target);
  bool8_t is_key = length == 64u;
  for (uint64_t i = 0u; is_key && i < length; ++i) {
    const char c = target[i];
    is_key = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  }
  if (is_key && !vkr_bakery_is_file(target)) {
    vkr_bakery_action_record_path(&cli->config, target, record_path,
                                  sizeof(record_path));
  } else {
    char hash[VKR_BAKERY_KEY_SIZE];
    if (!vkr_bakery_hash_file(target, hash, NULL)) {
      vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_IDX_MISSING_SOURCE, target, 0u,
                            0u, NULL, NULL);
      return VKR_BAKERY_EXIT_FAILED;
    }
    char actions[VKR_BAKERY_PATH_CAPACITY];
    (void)vkr_bakery_path_join(actions, sizeof(actions), cli->config.cache_dir,
                               "actions");
    VkrBakeryFindRecord find = {
        .config = &cli->config, .hash = hash, .directory = actions};
    (void)vkr_bakery_list_directory(actions, vkr_bakery_find_record_visit,
                                    &find);
    if (!find.found[0]) {
      vkr_bakery_print("%s (sha256 %s) was not produced by a cached action\n",
                       target, hash);
      return VKR_BAKERY_EXIT_FAILED;
    }
    (void)snprintf(record_path, sizeof(record_path), "%s", find.found);
  }
  uint8_t *data = NULL;
  uint64_t size = 0u;
  if (!vkr_bakery_read_file(record_path, MB(64), &data, &size)) {
    vkr_bakery_print("no action record %s\n", record_path);
    return VKR_BAKERY_EXIT_FAILED;
  }
  Arena *arena = (Arena *)cli->allocator.ctx;
  VkrBakeryJson *record = vkr_bakery_json_parse(arena, data, size, 64u, NULL);
  free(data);
  if (!record) {
    return VKR_BAKERY_EXIT_FAILED;
  }
  if (cli->config.json) {
    vkr_bakery_print_json(arena, record);
    return VKR_BAKERY_EXIT_OK;
  }
  int64_t wall = 0;
  int64_t cpu = 0;
  int64_t peak = 0;
  (void)vkr_bakery_json_get_int(record, "wall_ms", &wall);
  (void)vkr_bakery_json_get_int(record, "cpu_ms", &cpu);
  (void)vkr_bakery_json_get_int(record, "peak_rss_mib", &peak);
  vkr_bakery_print(
      "action   %s\nproducer %s v%lld (%s)\nsource   %s\n"
      "created  %s\ncost     %.2fs wall, %.2fs cpu, %lld MiB peak\n",
      vkr_bakery_recipe_string(record, "key", ""),
      vkr_bakery_recipe_string(record, "producer", ""),
      (long long)vkr_bakery_recipe_int(record, "producer_version", 0),
      vkr_bakery_recipe_string(record, "tool", ""),
      vkr_bakery_recipe_string(record, "source", ""),
      vkr_bakery_recipe_string(record, "created", ""), (float64_t)wall / 1000.0,
      (float64_t)cpu / 1000.0, (long long)peak);
  vkr_bakery_print("recipe\n");
  vkr_bakery_print_json(arena, vkr_bakery_json_get(record, "recipe"));
  const VkrBakeryJson *inputs = vkr_bakery_json_get(record, "inputs");
  vkr_bakery_print("inputs (%u)\n", inputs ? inputs->count : 0u);
  for (const VkrBakeryJson *input = inputs ? inputs->first : NULL; input;
       input = input->next) {
    vkr_bakery_print("  %.12s  %s\n",
                     vkr_bakery_recipe_string(input, "hash", ""),
                     vkr_bakery_recipe_string(input, "name", ""));
  }
  const VkrBakeryJson *depfile = vkr_bakery_json_get(record, "depfile");
  vkr_bakery_print("discovered inputs (%u)\n", depfile ? depfile->count : 0u);
  for (const VkrBakeryJson *entry = depfile ? depfile->first : NULL; entry;
       entry = entry->next) {
    const VkrBakeryJson *name = vkr_bakery_json_at(entry, 0u);
    const VkrBakeryJson *hash = vkr_bakery_json_at(entry, 1u);
    if (name && hash) {
      vkr_bakery_print("  %.12s  %s\n", (const char *)hash->string.str,
                       (const char *)name->string.str);
    }
  }
  const VkrBakeryJson *products = vkr_bakery_json_get(record, "products");
  vkr_bakery_print("products (%u)\n", products ? products->count : 0u);
  for (const VkrBakeryJson *product = products ? products->first : NULL;
       product; product = product->next) {
    const char *destination =
        vkr_bakery_recipe_string(product, "destination", NULL);
    vkr_bakery_print("  %.12s  %-8s %lld bytes%s%s\n",
                     vkr_bakery_recipe_string(product, "hash", ""),
                     vkr_bakery_recipe_string(product, "role", ""),
                     (long long)vkr_bakery_recipe_int(product, "bytes", 0),
                     destination ? " -> " : "", destination ? destination : "");
  }
  return VKR_BAKERY_EXIT_OK;
}

int vkr_bakery_cmd_status(VkrBakeryCli *cli) {
  if (cli->positional_count == 0u) {
    return vkr_bakery_usage("status needs sources, directories or manifests");
  }
  VkrBakeryGraph graph;
  if (!vkr_bakery_graph_init(&graph, &cli->config)) {
    return VKR_BAKERY_EXIT_ENVIRONMENT;
  }
  VkrBakeryJson *overrides = vkr_bakery_cli_overrides(cli, graph.arena);
  for (uint32_t i = 0u; i < cli->positional_count; ++i) {
    char extension[16];
    vkr_bakery_path_extension(cli->positional[i], extension, sizeof(extension));
    if (strcmp(extension, ".json") == 0 &&
        !strstr(cli->positional[i], ".recipe.json")) {
      (void)vkr_bakery_add_manifest(cli, &graph, cli->positional[i], overrides);
    }
  }
  VkrBakeryCli sources = *cli;
  sources.positional_count = 0u;
  for (uint32_t i = 0u; i < cli->positional_count; ++i) {
    char extension[16];
    vkr_bakery_path_extension(cli->positional[i], extension, sizeof(extension));
    if (strcmp(extension, ".json") != 0) {
      sources.positional[sources.positional_count++] = cli->positional[i];
    }
  }
  vkr_bakery_add_positionals(&sources, &graph, overrides);
  uint32_t counts[5] = {0};
  for (uint32_t i = 0u; i < graph.action_count; ++i) {
    VkrBakeryAction *action = graph.actions[i];
    const VkrBakeryProbe probe = vkr_bakery_graph_probe(&graph, action);
    counts[probe] += 1u;
    if (probe != VKR_BAKERY_PROBE_FRESH || cli->config.verbose) {
      vkr_bakery_print("%-12s %-10s %s\n", vkr_bakery_probe_names[probe],
                       action->producer->id, action->display);
    }
  }
  vkr_bakery_print("%u fresh, %u output-stale, %u stale, %u pending, %u "
                   "errors\n",
                   counts[0], counts[1], counts[2], counts[3], counts[4]);
  vkr_bakery_graph_shutdown(&graph);
  return graph.plan_failed || counts[4] ? VKR_BAKERY_EXIT_FAILED
                                        : VKR_BAKERY_EXIT_OK;
}

int vkr_bakery_cmd_gc(VkrBakeryCli *cli) {
  const uint64_t days = cli->older_than_days ? cli->older_than_days : 30u;
  VkrBakeryGcStats stats;
  if (!vkr_bakery_cache_gc(&cli->config, days, cli->config.dry_run, &stats)) {
    vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_CACHE_UNWRITABLE,
                          cli->config.cache_dir, 0u, 0u,
                          "another process holds the cache lock or the cache "
                          "is unreadable",
                          NULL);
    return VKR_BAKERY_EXIT_ENVIRONMENT;
  }
  vkr_bakery_print("%s%u action records removed, %u kept; %u products (%.1f "
                   "MiB) removed; %u staging directories removed\n",
                   cli->config.dry_run ? "would remove: " : "",
                   stats.records_removed, stats.records_kept,
                   stats.products_removed,
                   (float64_t)stats.bytes_removed / (1024.0 * 1024.0),
                   stats.staging_removed);
  return VKR_BAKERY_EXIT_OK;
}
