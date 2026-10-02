#pragma once

#include "vkr_bakery_internal.h"

#define VKR_BAKERY_EXIT_OK 0
#define VKR_BAKERY_EXIT_FAILED 1
#define VKR_BAKERY_EXIT_USAGE 2
#define VKR_BAKERY_EXIT_CANCELLED 3
#define VKR_BAKERY_EXIT_ENVIRONMENT 4

/* Parsed command line of one vkr_bakery invocation. Strings borrow argv. */
typedef struct VkrBakeryCli {
  const char *command;
  VkrBakeryConfig config;
  VkrAllocator allocator;
  const char *positional[256];
  uint32_t positional_count;
  const char *recipes[64];
  uint32_t recipe_count;
  const char *entries[256];
  uint32_t entry_count;
  const char *log_path;
  const char *cache_option;
  const char *root_option;
  const char *out;
  const char *producer;
  const char *backend;
  const char *build_config;
  const char *request;
  const char *result;
  const char *socket;
  const char *app;
  const char *shaders;
  /* Project packages: the game.json build profile and the player template
     directory (docs/proposals/project-packaging.md). */
  const char *profile;
  const char *player_template;
  /* `scripts`: the project library's name. */
  const char *name;
  uint64_t idle_exit_seconds;
  uint64_t older_than_days;
  bool8_t watch;
} VkrBakeryCli;

/** Parses `argv` (argv[0] the program, argv[1] the subcommand), runs the
 * subcommand and returns its exit code. Event lines also reach `sink`. The
 * daemon runs one command at a time through this. */
int vkr_bakery_run_command(int argc, char **argv, VkrBakeryEventSink sink,
                           void *sink_context);

int vkr_bakery_cmd_cook(VkrBakeryCli *cli);
int vkr_bakery_cmd_scripts(VkrBakeryCli *cli);
int vkr_bakery_cmd_serve(VkrBakeryCli *cli);
int vkr_bakery_cmd_send(VkrBakeryCli *cli);
int vkr_bakery_cmd_build(VkrBakeryCli *cli);
int vkr_bakery_cmd_shaders(VkrBakeryCli *cli);
int vkr_bakery_cmd_inspect(VkrBakeryCli *cli);
int vkr_bakery_cmd_explain(VkrBakeryCli *cli);
int vkr_bakery_cmd_status(VkrBakeryCli *cli);
int vkr_bakery_cmd_gc(VkrBakeryCli *cli);
int vkr_bakery_cmd_bundle(VkrBakeryCli *cli);

/** Reports a usage diagnostic and returns VKR_BAKERY_EXIT_USAGE. */
int vkr_bakery_usage(const char *message);
/** Runs `vkr_bakery tool <tool> <arguments>` in this process. */
int vkr_bakery_run_tool_inline(const char *tool, const char *const *arguments,
                               uint32_t count);

/** Probes an action without running it: resolves its key when its inputs
 * are available and reports whether the cache and its outputs are current. */
typedef enum VkrBakeryProbe {
  VKR_BAKERY_PROBE_FRESH = 0,
  VKR_BAKERY_PROBE_OUTPUT_STALE,
  VKR_BAKERY_PROBE_STALE,
  VKR_BAKERY_PROBE_PENDING,
  VKR_BAKERY_PROBE_ERROR,
} VkrBakeryProbe;

VkrBakeryProbe vkr_bakery_graph_probe(VkrBakeryGraph *graph,
                                      VkrBakeryAction *action);

typedef struct VkrBakeryGcStats {
  uint32_t records_removed;
  uint32_t records_kept;
  uint32_t products_removed;
  uint64_t bytes_removed;
  uint32_t staging_removed;
} VkrBakeryGcStats;

bool8_t vkr_bakery_cache_gc(const VkrBakeryConfig *config, uint64_t days,
                            bool8_t dry_run, VkrBakeryGcStats *out_stats);
