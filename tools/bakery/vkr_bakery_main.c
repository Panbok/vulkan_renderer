#include "vkr_bakery_commands.h"

#if VKR_BAKERY_WITH_COOKERS
#include "project/vkr_project_internal.h"
#include "vkr_bakery_bake.h"
#endif

#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_entry.h"
#include "platform/vkr_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* vkr_bakery: the asset build system (docs/proposals/asset-build-system.md).
 * One binary owns every producer, the cache and the event stream. */

typedef struct VkrBakeryCommand {
  const char *name;
  int (*run)(VkrBakeryCli *cli);
  const char *usage;
  const char *summary;
} VkrBakeryCommand;

vkr_internal int vkr_bakery_cmd_help(VkrBakeryCli *cli);

#if VKR_BAKERY_WITH_COOKERS
vkr_internal int vkr_bakery_cmd_project(VkrBakeryCli *cli) {
  vkr_bakery_install_cancel_signals();
  return vkr_project_main(&cli->config, cli->request, cli->result);
}
#endif

vkr_internal const VkrBakeryCommand vkr_bakery_commands[] = {
    {"cook", vkr_bakery_cmd_cook,
     "cook <source|directory>... [--producer <id>] [--recipe k=v]... [--out "
     "<path>]",
     "Cook sources with their producers; directories cook every supported "
     "file, and <module>.script.json builds a C script module."},
    {"build", vkr_bakery_cmd_build, "build <bakery.json>...",
     "Build every target of a bakery manifest."},
    {"shaders", vkr_bakery_cmd_shaders,
     "shaders [--backend vulkan|metal|all] [--entry <name>]... --out <dir> "
     "[--slangc <path>] [--watch]",
     "Compile shader libraries into a catalog the renderer loads."},
    {"inspect", vkr_bakery_cmd_inspect, "inspect <artifact|source>",
     "Describe a cooked artifact, or a source's action key and cache state."},
    {"explain", vkr_bakery_cmd_explain, "explain <artifact|action-key>",
     "Show why an artifact exists: recipe, inputs, timing and products."},
    {"status", vkr_bakery_cmd_status,
     "status <source|directory|bakery.json>...",
     "List fresh, stale and missing outputs without building."},
    {"serve", vkr_bakery_cmd_serve,
     "serve --root <dir> [--socket <path>] [--idle-exit <seconds>]",
     "Run the build daemon: requests over a local socket, file watches and "
     "background rebuilds."},
    {"send", vkr_bakery_cmd_send,
     "send [--root <dir>|--socket <path>] <request-json>",
     "Send one request to the daemon and print its events until the reply."},
    {"gc", vkr_bakery_cmd_gc, "gc [--older-than <days>] [--dry-run]",
     "Remove cache entries unused for the given number of days."},
#if VKR_BAKERY_WITH_COOKERS
    {"project", vkr_bakery_cmd_project,
     "project --request <request.json> --result <result.json>",
     "Run one managed project job: import, build, bake or delete (ADR-069)."},
    {"bake", NULL, "bake diffuse|probe [options]",
     "Bake a scene's diffuse volume or a reflection probe cubemap."},
    {"preview", NULL, "preview material|prune [options]",
     "Render a material thumbnail, or prune the workspace thumbnail cache."},
    {"bundle", vkr_bakery_cmd_bundle,
     "bundle <recipe.json> --out <dir> [--app <executable>] [--shaders "
     "<catalog>] [--dry-run]\n"
     "       vkr_bakery bundle <project directory> [--profile <name>] [--out "
     "<dir>] [--template <dir>] [--dry-run]",
     "Pack a scene's content into a .vkpak bundle with the runtime and "
     "shaders, or package a managed project as a standalone game."},
#endif
    {"tool", NULL, "tool <name> [arguments]",
     "Run a cooker's own command line (mesh, texture, font, ...)."},
    {"help", vkr_bakery_cmd_help, "help [<subcommand>|diag [<code>]|producers]",
     "Print help."},
};

vkr_internal const VkrBakeryCommand *vkr_bakery_find_command(const char *name) {
  for (uint32_t i = 0u; i < ArrayCount(vkr_bakery_commands); ++i) {
    if (strcmp(vkr_bakery_commands[i].name, name) == 0) {
      return &vkr_bakery_commands[i];
    }
  }
  return NULL;
}

vkr_internal void vkr_bakery_print_usage(FILE *stream) {
  fprintf(stream, "vkr_bakery - VKR asset and shader build system\n\n"
                  "usage: vkr_bakery <subcommand> [options] [arguments]\n\n");
  for (uint32_t i = 0u; i < ArrayCount(vkr_bakery_commands); ++i) {
    fprintf(stream, "  %-8s %s\n", vkr_bakery_commands[i].name,
            vkr_bakery_commands[i].summary);
  }
  fprintf(stream,
          "\nShared options:\n"
          "  --json                 JSON event lines on stdout\n"
          "  --log <file>           append JSON event lines to a file\n"
          "  --jobs <n>             worker count (default: logical cores)\n"
          "  --memory-budget <MiB>  concurrent peak-memory limit (default: "
          "half of RAM)\n"
          "  --cache <dir>          cache root (default: $VKR_BAKERY_CACHE or "
          "the user cache)\n"
          "  --root <dir>           repository or project root (default: "
          "current directory)\n"
          "  --platform <p>         host, macos-arm64 or windows-x64\n"
          "  --force                rebuild the named actions\n"
          "  --no-cache             neither read nor write cache records\n"
          "  --dry-run              plan and list work without running it\n"
          "  --quiet, --verbose     terminal verbosity\n\n"
          "Exit codes: 0 success, 1 action failed, 2 usage or recipe error, 3 "
          "cancelled, 4 environment error.\n");
}

vkr_internal int vkr_bakery_cmd_help(VkrBakeryCli *cli) {
  if (cli->positional_count == 0u) {
    vkr_bakery_print_usage(stdout);
    return 0;
  }
  const char *topic = cli->positional[0];
  if (strcmp(topic, "diag") == 0) {
    for (uint32_t i = 0u; i < vkr_bakery_diag_count(); ++i) {
      const VkrBakeryDiagInfo *info = vkr_bakery_diag_at(i);
      if (cli->positional_count > 1u &&
          strcmp(cli->positional[1], info->code) != 0) {
        continue;
      }
      printf("%-16s %-7s %s\n", info->code,
             info->severity == VKR_BAKERY_SEVERITY_ERROR ? "error" : "warning",
             info->description);
    }
    return 0;
  }
  if (strcmp(topic, "producers") == 0) {
    for (uint32_t i = 0u; i < vkr_bakery_producer_count(); ++i) {
      const VkrBakeryProducer *producer = vkr_bakery_producer_at(i);
      printf("%-16s v%u  %s\n", producer->id, producer->version,
             producer->summary);
      printf("%-16s     recipe fields:", "");
      for (const char *const *field = producer->recipe_fields; field && *field;
           ++field) {
        printf(" %s", *field);
      }
      printf("\n");
    }
    return 0;
  }
  const VkrBakeryCommand *command = vkr_bakery_find_command(topic);
  if (!command) {
    fprintf(stderr, "error VKR-CLI-0002: unknown subcommand '%s'\n", topic);
    return VKR_BAKERY_EXIT_USAGE;
  }
  printf("usage: vkr_bakery %s\n\n%s\n", command->usage, command->summary);
  if (strcmp(topic, "cook") == 0) {
    printf("\nRecipes: <source>.recipe.json beside a source overrides "
           "producer defaults; --recipe k=v overrides one field (JSON values "
           "accepted). \"producer\" selects the producer, \"output\" the "
           "published path. Run `vkr_bakery help producers` for fields.\n"
           "\nexample: vkr_bakery cook assets/textures/brick.png --recipe "
           "tier=preview\n");
  } else if (strcmp(topic, "shaders") == 0) {
    printf("\nRecipes: renderer/src/shaders/vulkan/slang/library.recipe.json "
           "and renderer/src/shaders/metal/library.recipe.json.\n"
           "\nexample: vkr_bakery shaders --backend vulkan --entry "
           "packet.world.frag --out build_release/shaders\n");
  }
  return 0;
}

// =============================================================================
// Argument parsing
// =============================================================================

vkr_internal bool8_t vkr_bakery_parse_u64(const char *text, uint64_t *out) {
  char *end = NULL;
  const unsigned long long value = strtoull(text, &end, 10);
  if (!text[0] || !end || *end) {
    return false_v;
  }
  *out = value;
  return true_v;
}

vkr_internal int vkr_bakery_usage_error(const char *message,
                                        const char *argument) {
  fprintf(stderr, "error VKR-CLI-0001: %s%s%s\n", message, argument ? ": " : "",
          argument ? argument : "");
  return VKR_BAKERY_EXIT_USAGE;
}

vkr_internal int vkr_bakery_parse(int argc, char **argv, VkrBakeryCli *cli) {
  VkrBakeryConfig *config = &cli->config;
  for (int i = 2; i < argc; ++i) {
    const char *argument = argv[i];
    const char *value = i + 1 < argc ? argv[i + 1] : NULL;
#define VKR_BAKERY_TAKE(field)                                                 \
  do {                                                                         \
    if (!value) {                                                              \
      return vkr_bakery_usage_error("missing value", argument);                \
    }                                                                          \
    field = value;                                                             \
    i += 1;                                                                    \
  } while (0)
    if (strcmp(argument, "--json") == 0) {
      config->json = true_v;
    } else if (strcmp(argument, "--quiet") == 0) {
      config->quiet = true_v;
    } else if (strcmp(argument, "--verbose") == 0) {
      config->verbose = true_v;
    } else if (strcmp(argument, "--force") == 0) {
      config->force = true_v;
    } else if (strcmp(argument, "--dry-run") == 0) {
      config->dry_run = true_v;
    } else if (strcmp(argument, "--no-cache") == 0) {
      config->no_cache = true_v;
    } else if (strcmp(argument, "--watch") == 0) {
      cli->watch = true_v;
    } else if (strcmp(argument, "--log") == 0) {
      VKR_BAKERY_TAKE(cli->log_path);
    } else if (strcmp(argument, "--cache") == 0) {
      VKR_BAKERY_TAKE(cli->cache_option);
    } else if (strcmp(argument, "--root") == 0) {
      VKR_BAKERY_TAKE(cli->root_option);
    } else if (strcmp(argument, "--out") == 0) {
      VKR_BAKERY_TAKE(cli->out);
    } else if (strcmp(argument, "--producer") == 0) {
      VKR_BAKERY_TAKE(cli->producer);
    } else if (strcmp(argument, "--backend") == 0) {
      VKR_BAKERY_TAKE(cli->backend);
    } else if (strcmp(argument, "--slangc") == 0) {
      const char *slangc = NULL;
      VKR_BAKERY_TAKE(slangc);
      (void)snprintf(config->slangc, sizeof(config->slangc), "%s", slangc);
    } else if (strcmp(argument, "--platform") == 0) {
      const char *platform = NULL;
      VKR_BAKERY_TAKE(platform);
      (void)snprintf(config->platform, sizeof(config->platform), "%s",
                     platform);
    } else if (strcmp(argument, "--config") == 0) {
      VKR_BAKERY_TAKE(cli->build_config);
    } else if (strcmp(argument, "--request") == 0) {
      VKR_BAKERY_TAKE(cli->request);
    } else if (strcmp(argument, "--result") == 0) {
      VKR_BAKERY_TAKE(cli->result);
    } else if (strcmp(argument, "--socket") == 0) {
      VKR_BAKERY_TAKE(cli->socket);
    } else if (strcmp(argument, "--app") == 0) {
      VKR_BAKERY_TAKE(cli->app);
    } else if (strcmp(argument, "--shaders") == 0) {
      VKR_BAKERY_TAKE(cli->shaders);
    } else if (strcmp(argument, "--profile") == 0) {
      VKR_BAKERY_TAKE(cli->profile);
    } else if (strcmp(argument, "--template") == 0) {
      VKR_BAKERY_TAKE(cli->player_template);
    } else if (strcmp(argument, "--jobs") == 0) {
      uint64_t jobs = 0u;
      if (!value || !vkr_bakery_parse_u64(value, &jobs) || jobs == 0u ||
          jobs > 256u) {
        return vkr_bakery_usage_error("--jobs expects 1..256", value);
      }
      config->jobs = (uint32_t)jobs;
      i += 1;
    } else if (strcmp(argument, "--memory-budget") == 0) {
      if (!value || !vkr_bakery_parse_u64(value, &config->memory_budget_mib) ||
          config->memory_budget_mib == 0u) {
        return vkr_bakery_usage_error("--memory-budget expects MiB", value);
      }
      i += 1;
    } else if (strcmp(argument, "--idle-exit") == 0) {
      if (!value || !vkr_bakery_parse_u64(value, &cli->idle_exit_seconds)) {
        return vkr_bakery_usage_error("--idle-exit expects seconds", value);
      }
      i += 1;
    } else if (strcmp(argument, "--older-than") == 0) {
      if (!value || !vkr_bakery_parse_u64(value, &cli->older_than_days)) {
        return vkr_bakery_usage_error("--older-than expects days", value);
      }
      i += 1;
    } else if (strcmp(argument, "--recipe") == 0) {
      if (!value || !strchr(value, '=') ||
          cli->recipe_count == ArrayCount(cli->recipes)) {
        return vkr_bakery_usage_error("--recipe expects key=value", value);
      }
      cli->recipes[cli->recipe_count++] = value;
      i += 1;
    } else if (strcmp(argument, "--entry") == 0) {
      if (!value || cli->entry_count == ArrayCount(cli->entries)) {
        return vkr_bakery_usage_error("--entry expects a shader name", value);
      }
      cli->entries[cli->entry_count++] = value;
      i += 1;
    } else if (argument[0] == '-' && argument[1] == '-') {
      return vkr_bakery_usage_error("unknown option", argument);
    } else {
      if (cli->positional_count == ArrayCount(cli->positional)) {
        return vkr_bakery_usage_error("too many arguments", argument);
      }
      cli->positional[cli->positional_count++] = argument;
    }
#undef VKR_BAKERY_TAKE
  }
  return 0;
}

vkr_internal bool8_t vkr_bakery_resolve_config(VkrBakeryCli *cli) {
  VkrBakeryConfig *config = &cli->config;
  if (!vkr_bakery_executable_path(config->self_path,
                                  sizeof(config->self_path))) {
    fprintf(stderr, "error: cannot locate the vkr_bakery executable\n");
    return false_v;
  }
  if (!vkr_bakery_path_absolute(cli->root_option ? cli->root_option : ".",
                                config->root, sizeof(config->root))) {
    return false_v;
  }
  const char *cache =
      cli->cache_option ? cli->cache_option : getenv("VKR_BAKERY_CACHE");
  char default_cache[VKR_BAKERY_PATH_CAPACITY];
  if (!cache || !cache[0]) {
    if (!vkr_bakery_user_cache_directory(default_cache,
                                         sizeof(default_cache))) {
      fprintf(stderr, "error VKR-CACHE-0001: no cache directory; pass "
                      "--cache\n");
      return false_v;
    }
    cache = default_cache;
  }
  if (!vkr_bakery_path_absolute(cache, config->cache_dir,
                                sizeof(config->cache_dir))) {
    return false_v;
  }
  if (!config->platform[0]) {
    (void)snprintf(config->platform, sizeof(config->platform), "host");
  }
  if (!config->jobs) {
    config->jobs = vkr_bakery_logical_cores();
  }
  if (!config->memory_budget_mib) {
    const uint64_t physical = vkr_bakery_physical_memory_bytes();
    config->memory_budget_mib =
        physical ? physical / 2u / (1024u * 1024u) : 4096u;
  }
  return true_v;
}

int vkr_bakery_run_command(int argc, char **argv, VkrBakeryEventSink sink,
                           void *sink_context) {
  const VkrBakeryCommand *command =
      argc >= 2 ? vkr_bakery_find_command(argv[1]) : NULL;
  if (!command || !command->run) {
    fprintf(stderr, "error VKR-CLI-0002: unknown subcommand '%s'\n",
            argc >= 2 ? argv[1] : "");
    return VKR_BAKERY_EXIT_USAGE;
  }
  VkrBakeryCli *cli = (VkrBakeryCli *)calloc(1u, sizeof(VkrBakeryCli));
  if (!cli) {
    return VKR_BAKERY_EXIT_ENVIRONMENT;
  }
  cli->command = command->name;
  int code = vkr_bakery_parse(argc, argv, cli);
  if (code == 0 && !vkr_bakery_resolve_config(cli)) {
    code = VKR_BAKERY_EXIT_ENVIRONMENT;
  }
  Arena *arena = NULL;
  if (code == 0) {
    arena = arena_create(MB(64), MB(1));
    cli->allocator.ctx = arena;
    const VkrBakeryEventsConfig events = {
        .json = cli->config.json,
        .quiet = cli->config.quiet,
        .verbose = cli->config.verbose,
    };
    if (!arena || !vkr_allocator_arena(&cli->allocator) ||
        !vkr_bakery_events_init(&cli->allocator, &events)) {
      code = VKR_BAKERY_EXIT_ENVIRONMENT;
    } else if (cli->log_path && !vkr_bakery_events_open_log(cli->log_path)) {
      fprintf(stderr, "error: cannot open log %s\n", cli->log_path);
      code = VKR_BAKERY_EXIT_ENVIRONMENT;
    }
    vkr_bakery_events_set_sink(sink, sink_context);
  }
  if (code == 0) {
    code = command->run(cli);
    vkr_bakery_events_shutdown();
  }
  if (arena) {
    vkr_allocator_release_global_accounting(&cli->allocator);
    arena_destroy(arena);
  }
  free(cli);
  return code;
}

VKR_MAIN(argc, argv) {
  if (argc >= 2 && strcmp(argv[1], "tool") == 0) {
    if (!vkr_platform_init()) {
      return VKR_BAKERY_EXIT_ENVIRONMENT;
    }
    const int code = vkr_bakery_tool_main(argc - 1, argv + 1);
    vkr_platform_shutdown();
    return code;
  }
#if VKR_BAKERY_WITH_COOKERS
  if (argc >= 2 &&
      (strcmp(argv[1], "bake") == 0 || strcmp(argv[1], "preview") == 0)) {
    if (!vkr_platform_init()) {
      return VKR_BAKERY_EXIT_ENVIRONMENT;
    }
    VkrBakeryConfig *config = (VkrBakeryConfig *)calloc(1u, sizeof(*config));
    int code = VKR_BAKERY_EXIT_ENVIRONMENT;
    if (config && vkr_bakery_executable_path(config->self_path,
                                             sizeof(config->self_path))) {
      code = strcmp(argv[1], "bake") == 0
                 ? vkr_bakery_bake_main(config, argc - 2, argv + 2)
                 : vkr_bakery_preview_main(config, argc - 2, argv + 2);
    }
    free(config);
    vkr_platform_shutdown();
    return code;
  }
#endif
  if (argc < 2 || strcmp(argv[1], "--help") == 0 ||
      strcmp(argv[1], "-h") == 0) {
    vkr_bakery_print_usage(argc < 2 ? stderr : stdout);
    return argc < 2 ? VKR_BAKERY_EXIT_USAGE : 0;
  }
  if (!vkr_bakery_find_command(argv[1])) {
    fprintf(stderr,
            "error VKR-CLI-0002: unknown subcommand '%s'; run `vkr_bakery "
            "help`\n",
            argv[1]);
    return VKR_BAKERY_EXIT_USAGE;
  }
  if (!vkr_platform_init()) {
    return VKR_BAKERY_EXIT_ENVIRONMENT;
  }
  const int code = vkr_bakery_run_command(argc, argv, NULL, NULL);
  vkr_platform_shutdown();
  return code;
}
