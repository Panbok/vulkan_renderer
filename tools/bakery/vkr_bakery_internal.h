#pragma once

#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "defines.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_allocator.h"
#include "vkr_bakery_diag.h"
#include "vkr_bakery_events.h"
#include "vkr_bakery_json.h"
#include "vkr_bakery_os.h"

/* Shared model of one vkr_bakery run: configuration, the action graph, the
 * content-addressed cache and the producer interface. The main thread plans
 * actions, resolves keys, publishes products and emits events; worker threads
 * only run producers and write into their own action. */

#define VKR_BAKERY_MAX_OUTPUTS 8u
#define VKR_BAKERY_KEY_SIZE VKR_BAKERY_SHA256_HEX
#define VKR_BAKERY_CACHE_VERSION "1"
/* Hashed into every action key. Format 2 records depfile prerequisites under
 * the build root relative to it; format 1 records stored absolute paths, which
 * let another checkout reuse products built from different sources, so their
 * keys are left unreachable. */
#define VKR_BAKERY_KEY_FORMAT 2

#if !defined(VKR_BAKERY_WITH_COOKERS)
#define VKR_BAKERY_WITH_COOKERS 0
#endif

/** Set by SIGINT/SIGTERM; every scheduler, child and job polls it. */
extern VkrAtomicBool vkr_bakery_cancel_requested;
/** Routes SIGINT and SIGTERM to vkr_bakery_cancel_requested, or to
 * vkr_bakery_signal_owner when a long-lived caller (the daemon) owns them;
 * that handler must also request cancellation. */
extern void (*vkr_bakery_signal_owner)(int);
void vkr_bakery_install_cancel_signals(void);

// =============================================================================
// Configuration
// =============================================================================

typedef struct VkrBakeryConfig {
  char self_path[VKR_BAKERY_PATH_CAPACITY];
  char cache_dir[VKR_BAKERY_PATH_CAPACITY];
  /** Root for portable display names; the repository or project directory. */
  char root[VKR_BAKERY_PATH_CAPACITY];
  char platform[32];
  /* External compilers; resolved by the shaders planner when empty. */
  char slangc[VKR_BAKERY_PATH_CAPACITY];
  uint32_t jobs;
  uint64_t memory_budget_mib;
  bool8_t force;
  bool8_t dry_run;
  bool8_t json;
  bool8_t quiet;
  bool8_t verbose;
  bool8_t no_cache;
} VkrBakeryConfig;

// =============================================================================
// Source index
// =============================================================================

typedef struct VkrBakeryIndex VkrBakeryIndex;

VkrBakeryIndex *vkr_bakery_index_open(const char *cache_dir);
/** Hash of a file, reusing the stored value while size, mtime and file
 * identity are unchanged. Main thread only. */
bool8_t vkr_bakery_index_hash(VkrBakeryIndex *index, const char *path,
                              char out_hash[VKR_BAKERY_KEY_SIZE],
                              uint64_t *out_size);
/** Brings the stored hashes of `paths` up to date, hashing the files whose
 * size, mtime or identity changed on worker threads; later
 * vkr_bakery_index_hash calls then find them. Main thread only. */
void vkr_bakery_index_prefetch(VkrBakeryIndex *index, const char *const *paths,
                               uint32_t count);
/** Carries the stored hashes of `from` and every path below it over to the
 * same paths below `to`, after a rename that keeps file identity and mtime.
 * Main thread only. */
void vkr_bakery_index_move_tree(VkrBakeryIndex *index, const char *from,
                                const char *to);
/** Records a known hash for a file just written by this process. */
void vkr_bakery_index_record(VkrBakeryIndex *index, const char *path,
                             const char *hash);
void vkr_bakery_index_close(VkrBakeryIndex *index);

// =============================================================================
// Actions
// =============================================================================

typedef struct VkrBakeryProducer VkrBakeryProducer;

typedef struct VkrBakeryInput {
  const char *path; /* Absolute host path; NULL for a dependency product. */
  const char *name; /* Portable key name, stable across checkouts. */
  char hash[VKR_BAKERY_KEY_SIZE];
  uint64_t size;
  int32_t dep_action; /* Index of the producing action, or -1. */
  const char *dep_role;
} VkrBakeryInput;

typedef struct VkrBakeryOutput {
  const char *role;
  const char *path; /* Absolute publication path. */
} VkrBakeryOutput;

typedef struct VkrBakeryProduct {
  char role[32];
  char hash[VKR_BAKERY_KEY_SIZE];
  uint64_t bytes;
  char path[VKR_BAKERY_PATH_CAPACITY]; /* Staged, then its CAS path. */
  /* Optional malloc-owned publication path for a side output the producer
   * discovered while running, such as a generated material. */
  char *destination;
} VkrBakeryProduct;

typedef enum VkrBakeryActionState {
  VKR_BAKERY_STATE_PLANNED = 0,
  VKR_BAKERY_STATE_READY,
  VKR_BAKERY_STATE_RUNNING,
  VKR_BAKERY_STATE_FINISHED, /* Worker done; main thread not yet published. */
  VKR_BAKERY_STATE_DONE,
} VkrBakeryActionState;

typedef enum VkrBakeryPriority {
  VKR_BAKERY_PRIORITY_INTERACTIVE = 0,
  VKR_BAKERY_PRIORITY_BUILD,
  VKR_BAKERY_PRIORITY_BACKGROUND,
} VkrBakeryPriority;

typedef struct VkrBakeryAction {
  uint32_t index;
  uint32_t id; /* Event id, assigned when the action starts or hits. */
  const VkrBakeryProducer *producer;
  VkrBakeryJson *recipe;
  const char *source;  /* Absolute primary source, may be NULL. */
  const char *display; /* Portable display name. */
  const char *label;
  /* Caller-selected publication path for the primary product, or NULL for
   * the producer's default next to the source. */
  const char *requested_output;
  /* Optional existing artifact a verifying tool may adopt instead of
   * recooking, such as a pre-cooked .vkt beside an imported source. It is
   * not part of the key; the tool checks it against the recipe. */
  const char *seed_path;
  VkrBakeryPriority priority;
  bool8_t force;
  bool8_t optional; /* A failure is reported as a warning, not an error. */
  /* Takes one of the scheduler's few whole-machine slots. Starts from the
   * producer's VKR_BAKERY_PRODUCER_EXCLUSIVE_CORES; a plan may clear it for
   * an action that stays on one core. */
  bool8_t exclusive_cores;

  VkrBakeryInput *inputs;
  uint32_t input_count;
  uint32_t input_capacity;
  VkrBakeryOutput outputs[VKR_BAKERY_MAX_OUTPUTS];
  uint32_t output_count;
  uint32_t *dependents;
  uint32_t dependent_count;
  uint32_t dependent_capacity;
  uint32_t pending_deps;

  char prekey[VKR_BAKERY_KEY_SIZE];
  char key[VKR_BAKERY_KEY_SIZE];
  uint64_t est_peak_mib;

  /* Written by the worker while RUNNING, read by main after FINISHED. */
  VkrBakeryActionState state;
  VkrBakeryActionStatus status;
  bool8_t cached;
  bool8_t published;
  VkrBakeryProduct *products; /* malloc-owned, grows while running. */
  uint32_t product_count;
  uint32_t product_capacity;
  char **discovered; /* Absolute paths, malloc-owned. */
  uint32_t discovered_count;
  uint32_t discovered_capacity;
  uint64_t wall_ms;
  uint64_t cpu_ms;
  uint64_t peak_rss_bytes;
  const char *peak_rss_source;
  float64_t started_seconds;
  char staging[VKR_BAKERY_PATH_CAPACITY];
  char stdout_path[VKR_BAKERY_PATH_CAPACITY];
  char stderr_path[VKR_BAKERY_PATH_CAPACITY];
  uint64_t stdout_offset;
  uint64_t stderr_offset;
  char failure[256];
  /* Producer-owned data set during planning (arena storage). */
  void *producer_data;
} VkrBakeryAction;

// =============================================================================
// Graph
// =============================================================================

typedef struct VkrBakeryGraph {
  const VkrBakeryConfig *config;
  Arena *arena; /* Main-thread planning storage. */
  VkrAllocator allocator;
  VkrBakeryIndex *index;
  /* False when a caller lent the index (vkr_bakery_graph_use_index). */
  bool8_t owns_index;
  VkrBakeryAction **actions;
  uint32_t action_count;
  uint32_t action_capacity;
  uint32_t root_count;
  uint32_t next_event_id;
  bool8_t plan_failed;
} VkrBakeryGraph;

bool8_t vkr_bakery_graph_init(VkrBakeryGraph *graph,
                              const VkrBakeryConfig *config);
void vkr_bakery_graph_shutdown(VkrBakeryGraph *graph);
/** Plans against a caller's source index instead of the graph's own, so one
 * process keeps one view of `<cache>/index`; the caller closes it. */
void vkr_bakery_graph_use_index(VkrBakeryGraph *graph, VkrBakeryIndex *index);

/** Creates and plans one action. `recipe` may be NULL (producer defaults).
 * Planning failures are reported as diagnostics and return NULL. */
VkrBakeryAction *vkr_bakery_graph_add(VkrBakeryGraph *graph,
                                      const VkrBakeryProducer *producer,
                                      const char *source, VkrBakeryJson *recipe,
                                      const char *requested_output);

/** Resolves keys, runs pending actions and publishes. Returns true when no
 * required action failed. */
bool8_t vkr_bakery_graph_execute(VkrBakeryGraph *graph);

/* Planning helpers for producers (main thread). */
const char *vkr_bakery_graph_strdup(VkrBakeryGraph *graph, const char *text);
const char *vkr_bakery_graph_printf(VkrBakeryGraph *graph, const char *format,
                                    ...);
/** Portable name of `path` relative to the config root, else absolute. */
const char *vkr_bakery_graph_display(VkrBakeryGraph *graph, const char *path);
bool8_t vkr_bakery_action_input(VkrBakeryGraph *graph, VkrBakeryAction *action,
                                const char *path, const char *name);
bool8_t vkr_bakery_action_dep_input(VkrBakeryGraph *graph,
                                    VkrBakeryAction *action,
                                    VkrBakeryAction *dependency,
                                    const char *role, const char *name);
bool8_t vkr_bakery_action_output(VkrBakeryGraph *graph, VkrBakeryAction *action,
                                 const char *role, const char *path);
void vkr_bakery_action_label(VkrBakeryGraph *graph, VkrBakeryAction *action,
                             const char *format, ...);
/** Reports a planning diagnostic against the action's source. */
void vkr_bakery_plan_diag(VkrBakeryGraph *graph, VkrBakeryAction *action,
                          VkrBakeryDiag diag, const char *format, ...);

// =============================================================================
// Recipes
// =============================================================================

/** Loads `<source>.recipe.json` when present, overlays `overrides`, checks
 * fields against the producer and returns the merged object. */
VkrBakeryJson *vkr_bakery_recipe_load(VkrBakeryGraph *graph,
                                      const VkrBakeryProducer *producer,
                                      const char *source,
                                      const VkrBakeryJson *overrides);
bool8_t vkr_bakery_recipe_check(VkrBakeryGraph *graph,
                                const VkrBakeryProducer *producer,
                                const char *source,
                                const VkrBakeryJson *recipe);
const char *vkr_bakery_recipe_string(const VkrBakeryJson *recipe,
                                     const char *key, const char *fallback);
int64_t vkr_bakery_recipe_int(const VkrBakeryJson *recipe, const char *key,
                              int64_t fallback);
float64_t vkr_bakery_recipe_number(const VkrBakeryJson *recipe, const char *key,
                                   float64_t fallback);
bool8_t vkr_bakery_recipe_bool(const VkrBakeryJson *recipe, const char *key,
                               bool8_t fallback);

// =============================================================================
// Task: one running action on a worker thread
// =============================================================================

typedef struct VkrBakeryTask {
  const VkrBakeryConfig *config;
  VkrBakeryAction *action;
  Arena *arena; /* Worker scratch, reset per task. */
  VkrAtomicBool *cancel;
} VkrBakeryTask;

bool8_t vkr_bakery_task_cancelled(const VkrBakeryTask *task);
/** Runs `vkr_bakery tool <tool> args...` with output captured to the
 * action's logs and usage accounted. */
bool8_t vkr_bakery_task_tool(VkrBakeryTask *task, const char *tool,
                             const char *const *arguments, uint32_t count,
                             int32_t *out_exit_code);
/** Runs an external executable through `vkr_bakery tool exec`. */
bool8_t vkr_bakery_task_process(VkrBakeryTask *task, const char *executable,
                                const char *const *arguments, uint32_t count,
                                int32_t *out_exit_code);
/** Path for a staged file named `name` inside the action's staging dir. */
const char *vkr_bakery_task_stage(VkrBakeryTask *task, const char *name);
bool8_t vkr_bakery_task_product(VkrBakeryTask *task, const char *role,
                                const char *staged_path);
/** Adds a product that is also published to `destination` on every hit. */
bool8_t vkr_bakery_task_side_product(VkrBakeryTask *task, const char *role,
                                     const char *staged_path,
                                     const char *destination);
/** Appends one product slot; NULL when out of memory. Worker or main. */
VkrBakeryProduct *vkr_bakery_action_new_product(VkrBakeryAction *action);
void vkr_bakery_task_discovered(VkrBakeryTask *task, const char *path);
void vkr_bakery_task_progress(VkrBakeryTask *task, float64_t fraction,
                              const char *detail);
void vkr_bakery_task_diag(VkrBakeryTask *task, VkrBakeryDiag diag,
                          const char *source, uint32_t line, uint32_t column,
                          const char *message, const char *hint);
void vkr_bakery_task_fail(VkrBakeryTask *task, const char *format, ...);
/** Reads the tail of the action's stderr (and stdout) for failure context. */
const char *vkr_bakery_task_output_tail(VkrBakeryTask *task,
                                        uint32_t max_bytes);
/** Reports every prerequisite of a Makefile depfile below the root as a
 * discovered input; the recipe's tool version keys toolchain headers. */
void vkr_bakery_task_read_depfile(VkrBakeryTask *task, const char *path);
/** Parses `file:line:col: severity: message` lines from captured output. */
uint32_t vkr_bakery_task_parse_compiler_diags(VkrBakeryTask *task,
                                              VkrBakeryDiag error_diag,
                                              VkrBakeryDiag warning_diag);

// =============================================================================
// Producers
// =============================================================================

typedef enum VkrBakeryProducerFlags {
  VKR_BAKERY_PRODUCER_EXCLUSIVE_CORES = 1u << 0,
} VkrBakeryProducerFlags;

struct VkrBakeryProducer {
  const char *id;
  uint32_t version;
  const char *identity;
  uint32_t flags;
  const char *summary;
  const char *const *recipe_fields; /* NULL-terminated accepted keys. */
  /** Declares inputs and outputs and validates the recipe. */
  bool8_t (*plan)(VkrBakeryGraph *graph, VkrBakeryAction *action);
  uint64_t (*estimate_peak_mib)(const VkrBakeryAction *action);
  bool8_t (*run)(VkrBakeryTask *task);
};

const VkrBakeryProducer *vkr_bakery_producer_find(const char *id);
uint32_t vkr_bakery_producer_count(void);
const VkrBakeryProducer *vkr_bakery_producer_at(uint32_t index);
/** Producer for a source path by extension, or NULL. */
const VkrBakeryProducer *vkr_bakery_producer_for_source(const char *path);

#if VKR_BAKERY_WITH_COOKERS
extern const VkrBakeryProducer vkr_bakery_producer_texture;
extern const VkrBakeryProducer vkr_bakery_producer_mesh;
extern const VkrBakeryProducer vkr_bakery_producer_animation;
extern const VkrBakeryProducer vkr_bakery_producer_collision;
extern const VkrBakeryProducer vkr_bakery_producer_font;
extern const VkrBakeryProducer vkr_bakery_producer_table;
#endif
extern const VkrBakeryProducer vkr_bakery_producer_shader_spirv;
extern const VkrBakeryProducer vkr_bakery_producer_shader_msl;
extern const VkrBakeryProducer vkr_bakery_producer_shader_concat;
extern const VkrBakeryProducer vkr_bakery_producer_shader_metallib;
extern const VkrBakeryProducer vkr_bakery_producer_shader_manifest;
extern const VkrBakeryProducer vkr_bakery_producer_script_object;
extern const VkrBakeryProducer vkr_bakery_producer_script_library;

/** Plans a C script module from its `<module>.script.json` description: one
 * `script_object` action per translation unit and one `script_library`
 * action publishing the hot-reload library and the static archive into
 * `output_directory` (the description's directory when NULL). */
VkrBakeryAction *vkr_bakery_plan_script(VkrBakeryGraph *graph,
                                        const char *description,
                                        const char *output_directory);

// =============================================================================
// Cache
// =============================================================================

bool8_t vkr_bakery_cache_prepare(const VkrBakeryConfig *config);
/** Looks up the full key for `prekey` by rehashing recorded depfiles. */
bool8_t vkr_bakery_cache_lookup(VkrBakeryGraph *graph, VkrBakeryAction *action);
/** Moves staged products into the CAS and records the result. Worker safe. */
bool8_t vkr_bakery_cache_store(const VkrBakeryConfig *config,
                               VkrBakeryAction *action, Arena *scratch);
void vkr_bakery_cas_path(const VkrBakeryConfig *config, const char *hash,
                         char *out, uint32_t capacity);
void vkr_bakery_action_record_path(const VkrBakeryConfig *config,
                                   const char *key, char *out,
                                   uint32_t capacity);
/** Computes a full key from the prekey and depfile entry list. */
void vkr_bakery_full_key(const char *prekey, const VkrBakeryJson *depfile,
                         Arena *arena, char out_key[VKR_BAKERY_KEY_SIZE]);

// =============================================================================
// Tool dispatch (the `tool` subcommand)
// =============================================================================

typedef int (*VkrBakeryToolEntry)(int argc, char **argv);

typedef struct VkrBakeryTool {
  const char *name;
  VkrBakeryToolEntry entry;
  const char *summary;
} VkrBakeryTool;

const VkrBakeryTool *vkr_bakery_tool_find(const char *name);
uint32_t vkr_bakery_tool_count(void);
const VkrBakeryTool *vkr_bakery_tool_at(uint32_t index);
int vkr_bakery_tool_main(int argc, char **argv);
