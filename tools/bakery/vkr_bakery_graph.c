#include "filesystem/filesystem.h"
#include "vkr_bakery_commands.h"

#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_platform.h"
#include "vkr_bakery_buffer.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One process-wide cancellation flag: SIGINT/SIGTERM (and SIGBREAK, Windows'
 * Ctrl+Break) and the daemon set it, tasks poll it and process_run stops
 * running tools when it is raised. */
VkrAtomicBool vkr_bakery_cancel_requested = false;

#define VKR_BAKERY_TOOL_TIMEOUT_MS (2u * 60u * 60u * 1000u)
#define VKR_BAKERY_LOG_LINE_MAX 4096u

// =============================================================================
// Graph lifetime and planning helpers
// =============================================================================

bool8_t vkr_bakery_graph_init(VkrBakeryGraph *graph,
                              const VkrBakeryConfig *config) {
  MemZero(graph, sizeof(*graph));
  graph->config = config;
  graph->arena = arena_create(GB(4), MB(4));
  if (!graph->arena) {
    return false_v;
  }
  graph->allocator.ctx = graph->arena;
  if (!vkr_allocator_arena(&graph->allocator)) {
    arena_destroy(graph->arena);
    return false_v;
  }
  graph->index = vkr_bakery_index_open(config->cache_dir);
  if (!graph->index) {
    arena_destroy(graph->arena);
    return false_v;
  }
  graph->owns_index = true_v;
  graph->next_event_id = 1u;
  return true_v;
}

void vkr_bakery_graph_shutdown(VkrBakeryGraph *graph) {
  for (uint32_t i = 0u; i < graph->action_count; ++i) {
    VkrBakeryAction *action = graph->actions[i];
    for (uint32_t d = 0u; d < action->discovered_count; ++d) {
      free(action->discovered[d]);
    }
    free(action->discovered);
    for (uint32_t p = 0u; p < action->product_count; ++p) {
      free(action->products[p].destination);
    }
    free(action->products);
    free(action->inputs);
    free(action->dependents);
  }
  free(graph->actions);
  if (graph->owns_index) {
    vkr_bakery_index_close(graph->index);
  }
  vkr_allocator_release_global_accounting(&graph->allocator);
  arena_destroy(graph->arena);
  MemZero(graph, sizeof(*graph));
}

void vkr_bakery_graph_use_index(VkrBakeryGraph *graph, VkrBakeryIndex *index) {
  if (!index || index == graph->index) {
    return;
  }
  if (graph->owns_index) {
    vkr_bakery_index_close(graph->index);
  }
  graph->index = index;
  graph->owns_index = false_v;
}

const char *vkr_bakery_graph_strdup(VkrBakeryGraph *graph, const char *text) {
  if (!text) {
    return NULL;
  }
  const uint64_t length = strlen(text);
  char *copy =
      (char *)arena_alloc(graph->arena, length + 1u, ARENA_MEMORY_TAG_STRING);
  if (copy) {
    MemCopy(copy, text, length + 1u);
  }
  return copy;
}

const char *vkr_bakery_graph_printf(VkrBakeryGraph *graph, const char *format,
                                    ...) {
  char text[VKR_BAKERY_PATH_CAPACITY];
  va_list arguments;
  va_start(arguments, format);
  (void)vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  return vkr_bakery_graph_strdup(graph, text);
}

const char *vkr_bakery_graph_display(VkrBakeryGraph *graph, const char *path) {
  if (!path) {
    return NULL;
  }
  char relative[VKR_BAKERY_PATH_CAPACITY];
  if (graph->config->root[0] &&
      vkr_bakery_path_relative(graph->config->root, path, relative,
                               sizeof(relative))) {
    return vkr_bakery_graph_strdup(graph, relative);
  }
  char absolute[VKR_BAKERY_PATH_CAPACITY];
  if (vkr_bakery_path_absolute(path, absolute, sizeof(absolute))) {
    return vkr_bakery_graph_strdup(graph, absolute);
  }
  return vkr_bakery_graph_strdup(graph, path);
}

vkr_internal VkrBakeryInput *
vkr_bakery_action_new_input(VkrBakeryAction *action) {
  if (action->input_count == action->input_capacity) {
    const uint32_t capacity =
        action->input_capacity ? action->input_capacity * 2u : 8u;
    VkrBakeryInput *inputs = (VkrBakeryInput *)realloc(
        action->inputs, sizeof(VkrBakeryInput) * capacity);
    if (!inputs) {
      return NULL;
    }
    action->inputs = inputs;
    action->input_capacity = capacity;
  }
  VkrBakeryInput *input = &action->inputs[action->input_count++];
  MemZero(input, sizeof(*input));
  input->dep_action = -1;
  return input;
}

bool8_t vkr_bakery_action_input(VkrBakeryGraph *graph, VkrBakeryAction *action,
                                const char *path, const char *name) {
  char absolute[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bakery_path_absolute(path, absolute, sizeof(absolute))) {
    return false_v;
  }
  for (uint32_t i = 0u; i < action->input_count; ++i) {
    if (action->inputs[i].path &&
        strcmp(action->inputs[i].path, absolute) == 0) {
      return true_v;
    }
  }
  VkrBakeryInput *input = vkr_bakery_action_new_input(action);
  if (!input) {
    return false_v;
  }
  input->path = vkr_bakery_graph_strdup(graph, absolute);
  input->name = vkr_bakery_graph_strdup(graph, name ? name : absolute);
  return input->path && input->name;
}

bool8_t vkr_bakery_action_dep_input(VkrBakeryGraph *graph,
                                    VkrBakeryAction *action,
                                    VkrBakeryAction *dependency,
                                    const char *role, const char *name) {
  VkrBakeryInput *input = vkr_bakery_action_new_input(action);
  if (!input) {
    return false_v;
  }
  input->dep_action = (int32_t)dependency->index;
  input->dep_role = vkr_bakery_graph_strdup(graph, role);
  input->name = vkr_bakery_graph_strdup(graph, name);
  if (dependency->dependent_count == dependency->dependent_capacity) {
    const uint32_t capacity = dependency->dependent_capacity
                                  ? dependency->dependent_capacity * 2u
                                  : 4u;
    uint32_t *dependents = (uint32_t *)realloc(dependency->dependents,
                                               sizeof(uint32_t) * capacity);
    if (!dependents) {
      return false_v;
    }
    dependency->dependents = dependents;
    dependency->dependent_capacity = capacity;
  }
  /* Planned actions get their index when they join the graph. */
  dependency->dependents[dependency->dependent_count++] = action->index;
  action->pending_deps += 1u;
  return true_v;
}

bool8_t vkr_bakery_action_output(VkrBakeryGraph *graph, VkrBakeryAction *action,
                                 const char *role, const char *path) {
  if (action->output_count == VKR_BAKERY_MAX_OUTPUTS) {
    return false_v;
  }
  char absolute[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bakery_path_absolute(path, absolute, sizeof(absolute))) {
    return false_v;
  }
  VkrBakeryOutput *output = &action->outputs[action->output_count++];
  output->role = vkr_bakery_graph_strdup(graph, role);
  output->path = vkr_bakery_graph_strdup(graph, absolute);
  return output->role && output->path;
}

void vkr_bakery_action_label(VkrBakeryGraph *graph, VkrBakeryAction *action,
                             const char *format, ...) {
  char text[256];
  va_list arguments;
  va_start(arguments, format);
  (void)vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  action->label = vkr_bakery_graph_strdup(graph, text);
}

void vkr_bakery_plan_diag(VkrBakeryGraph *graph, VkrBakeryAction *action,
                          VkrBakeryDiag diag, const char *format, ...) {
  char text[1024];
  va_list arguments;
  va_start(arguments, format);
  (void)vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  vkr_bakery_event_diag(0u, diag,
                        action && action->display ? action->display : NULL, 0u,
                        0u, text, NULL);
  const VkrBakeryDiagInfo *info = vkr_bakery_diag_info(diag);
  if (info->severity == VKR_BAKERY_SEVERITY_ERROR) {
    graph->plan_failed = true_v;
  }
}

VkrBakeryAction *vkr_bakery_graph_add(VkrBakeryGraph *graph,
                                      const VkrBakeryProducer *producer,
                                      const char *source, VkrBakeryJson *recipe,
                                      const char *requested_output) {
  VkrBakeryAction *action = (VkrBakeryAction *)arena_alloc(
      graph->arena, sizeof(VkrBakeryAction), ARENA_MEMORY_TAG_STRUCT);
  if (!action) {
    graph->plan_failed = true_v;
    return NULL;
  }
  MemZero(action, sizeof(*action));
  action->index = graph->action_count;
  action->producer = producer;
  action->recipe = recipe ? recipe : vkr_bakery_json_object(graph->arena);
  action->priority = VKR_BAKERY_PRIORITY_BUILD;
  action->force = graph->config->force;
  action->exclusive_cores =
      (producer->flags & VKR_BAKERY_PRODUCER_EXCLUSIVE_CORES) != 0u;
  if (source) {
    char absolute[VKR_BAKERY_PATH_CAPACITY];
    if (!vkr_bakery_path_absolute(source, absolute, sizeof(absolute))) {
      graph->plan_failed = true_v;
      return NULL;
    }
    action->source = vkr_bakery_graph_strdup(graph, absolute);
    action->display = vkr_bakery_graph_display(graph, absolute);
  } else {
    action->display = producer->id;
  }
  if (requested_output) {
    char absolute[VKR_BAKERY_PATH_CAPACITY];
    if (!vkr_bakery_path_absolute(requested_output, absolute,
                                  sizeof(absolute))) {
      graph->plan_failed = true_v;
      return NULL;
    }
    action->requested_output = vkr_bakery_graph_strdup(graph, absolute);
  }
  if (!vkr_bakery_recipe_check(graph, producer, action->display,
                               action->recipe) ||
      (producer->plan && !producer->plan(graph, action))) {
    graph->plan_failed = true_v;
    return NULL;
  }
  if (!action->label) {
    action->label = "";
  }
  action->est_peak_mib =
      producer->estimate_peak_mib ? producer->estimate_peak_mib(action) : 256u;
  if (graph->action_count == graph->action_capacity) {
    const uint32_t capacity =
        graph->action_capacity ? graph->action_capacity * 2u : 64u;
    VkrBakeryAction **actions = (VkrBakeryAction **)realloc(
        graph->actions, sizeof(VkrBakeryAction *) * capacity);
    if (!actions) {
      graph->plan_failed = true_v;
      return NULL;
    }
    graph->actions = actions;
    graph->action_capacity = capacity;
  }
  graph->actions[graph->action_count++] = action;
  return action;
}

// =============================================================================
// Recipes
// =============================================================================

VkrBakeryJson *vkr_bakery_recipe_load(VkrBakeryGraph *graph,
                                      const VkrBakeryProducer *producer,
                                      const char *source,
                                      const VkrBakeryJson *overrides) {
  VkrBakeryJson *recipe = vkr_bakery_json_object(graph->arena);
  if (source) {
    char path[VKR_BAKERY_PATH_CAPACITY];
    (void)snprintf(path, sizeof(path), "%s.recipe.json", source);
    if (vkr_bakery_is_file(path)) {
      uint8_t *data = NULL;
      uint64_t length = 0u;
      VkrBakeryJsonError error;
      VkrBakeryJson *loaded = NULL;
      if (vkr_bakery_read_file(path, MB(4), &data, &length)) {
        loaded = vkr_bakery_json_parse(graph->arena, data, length, 32u, &error);
        free(data);
      }
      if (!loaded || loaded->type != VKR_BAKERY_JSON_OBJECT) {
        vkr_bakery_event_diag(
            0u, VKR_BAKERY_DIAG_REC_UNREADABLE,
            vkr_bakery_graph_display(graph, path), loaded ? 0u : error.line,
            loaded ? 0u : error.column,
            loaded ? "a recipe must be a JSON object" : error.message, NULL);
        graph->plan_failed = true_v;
        return NULL;
      }
      recipe = loaded;
    }
  }
  for (const VkrBakeryJson *field = overrides ? overrides->first : NULL; field;
       field = field->next) {
    vkr_bakery_json_set(graph->arena, recipe, (const char *)field->key.str,
                        vkr_bakery_json_clone(graph->arena, field));
  }
  (void)producer;
  return recipe;
}

bool8_t vkr_bakery_recipe_check(VkrBakeryGraph *graph,
                                const VkrBakeryProducer *producer,
                                const char *source,
                                const VkrBakeryJson *recipe) {
  bool8_t ok = true_v;
  for (const VkrBakeryJson *field = recipe ? recipe->first : NULL; field;
       field = field->next) {
    bool8_t known = false_v;
    for (const char *const *name = producer->recipe_fields; name && *name;
         ++name) {
      if (field->key.length == strlen(*name) &&
          MemCompare(field->key.str, *name, field->key.length) == 0) {
        known = true_v;
        break;
      }
    }
    if (!known) {
      char message[256];
      (void)snprintf(message, sizeof(message),
                     "producer '%s' does not accept recipe field '%.*s'",
                     producer->id, (int)field->key.length, field->key.str);
      vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_REC_UNKNOWN_FIELD, source, 0u,
                            0u, message, NULL);
      ok = false_v;
    }
  }
  if (!ok) {
    graph->plan_failed = true_v;
  }
  return ok;
}

const char *vkr_bakery_recipe_string(const VkrBakeryJson *recipe,
                                     const char *key, const char *fallback) {
  const VkrBakeryJson *value = vkr_bakery_json_get(recipe, key);
  return value && value->type == VKR_BAKERY_JSON_STRING
             ? (const char *)value->string.str
             : fallback;
}

int64_t vkr_bakery_recipe_int(const VkrBakeryJson *recipe, const char *key,
                              int64_t fallback) {
  int64_t value = 0;
  return vkr_bakery_json_get_int(recipe, key, &value) ? value : fallback;
}

float64_t vkr_bakery_recipe_number(const VkrBakeryJson *recipe, const char *key,
                                   float64_t fallback) {
  float64_t value = 0.0;
  return vkr_bakery_json_get_number(recipe, key, &value) ? value : fallback;
}

bool8_t vkr_bakery_recipe_bool(const VkrBakeryJson *recipe, const char *key,
                               bool8_t fallback) {
  bool8_t value = false_v;
  return vkr_bakery_json_get_bool(recipe, key, &value) ? value : fallback;
}

// =============================================================================
// Key derivation (main thread)
// =============================================================================

vkr_internal int vkr_bakery_compare_inputs(const void *lhs, const void *rhs) {
  const VkrBakeryInput *a = (const VkrBakeryInput *)lhs;
  const VkrBakeryInput *b = (const VkrBakeryInput *)rhs;
  return strcmp(a->name, b->name);
}

vkr_internal bool8_t vkr_bakery_resolve_key(VkrBakeryGraph *graph,
                                            VkrBakeryAction *action) {
  for (uint32_t i = 0u; i < action->input_count; ++i) {
    VkrBakeryInput *input = &action->inputs[i];
    if (input->dep_action >= 0) {
      const VkrBakeryAction *dependency =
          graph->actions[(uint32_t)input->dep_action];
      bool8_t found = false_v;
      for (uint32_t p = 0u; p < dependency->product_count; ++p) {
        if (strcmp(dependency->products[p].role, input->dep_role) == 0) {
          MemCopy(input->hash, dependency->products[p].hash,
                  VKR_BAKERY_KEY_SIZE);
          input->size = dependency->products[p].bytes;
          input->path = dependency->products[p].path;
          found = true_v;
          break;
        }
      }
      if (!found) {
        (void)snprintf(action->failure, sizeof(action->failure),
                       "dependency %s produced no '%s'", dependency->display,
                       input->dep_role);
        return false_v;
      }
      continue;
    }
    if (!vkr_bakery_index_hash(graph->index, input->path, input->hash,
                               &input->size)) {
      vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_IDX_MISSING_SOURCE,
                            vkr_bakery_graph_display(graph, input->path), 0u,
                            0u, NULL, NULL);
      (void)snprintf(action->failure, sizeof(action->failure),
                     "missing input %s", input->name);
      return false_v;
    }
  }
  if (action->input_count > 1u) {
    qsort(action->inputs, action->input_count, sizeof(VkrBakeryInput),
          vkr_bakery_compare_inputs);
  }
  Arena *arena = graph->arena;
  const uint64_t mark = arena_pos(arena);
  VkrBakeryJson *key = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, key, "format",
                      vkr_bakery_json_int(arena, VKR_BAKERY_KEY_FORMAT));
  vkr_bakery_json_set(arena, key, "producer",
                      vkr_bakery_json_cstr(arena, action->producer->id));
  vkr_bakery_json_set(arena, key, "version",
                      vkr_bakery_json_int(arena, action->producer->version));
  vkr_bakery_json_set(arena, key, "identity",
                      vkr_bakery_json_cstr(arena, action->producer->identity));
  vkr_bakery_json_set(arena, key, "platform",
                      vkr_bakery_json_cstr(arena, graph->config->platform));
  vkr_bakery_json_set(arena, key, "recipe", action->recipe);
  VkrBakeryJson *inputs = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < action->input_count; ++i) {
    VkrBakeryJson *pair = vkr_bakery_json_array(arena);
    vkr_bakery_json_append(pair,
                           vkr_bakery_json_cstr(arena, action->inputs[i].name));
    vkr_bakery_json_append(pair,
                           vkr_bakery_json_cstr(arena, action->inputs[i].hash));
    vkr_bakery_json_append(inputs, pair);
  }
  vkr_bakery_json_set(arena, key, "inputs", inputs);
  String8 text = {0};
  const bool8_t ok =
      vkr_bakery_json_write(arena, key, VKR_BAKERY_JSON_CANONICAL, &text);
  if (ok) {
    vkr_bakery_hash_bytes(text.str, text.length, action->prekey);
  }
  /* The recipe node stays referenced by the action, so only discard the
   * serialized text; node storage is small. */
  (void)mark;
  return ok;
}

// =============================================================================
// Scheduler state
// =============================================================================

typedef struct VkrBakeryScheduler {
  VkrBakeryGraph *graph;
  VkrMutex mutex;
  VkrCondVar wake;
  uint32_t *ready;
  uint32_t ready_count;
  uint32_t ready_capacity;
  uint64_t reserved_mib;
  uint32_t running;
  uint32_t exclusive_running;
  bool8_t shutdown;
  uint32_t outstanding;
  uint32_t ok;
  uint32_t failed;
  uint32_t cancelled;
  uint32_t cached;
} VkrBakeryScheduler;

typedef struct VkrBakeryWorker {
  VkrBakeryScheduler *scheduler;
  VkrThread thread;
  Arena *arena;
} VkrBakeryWorker;

vkr_internal bool8_t vkr_bakery_push_ready(VkrBakeryScheduler *scheduler,
                                           VkrBakeryAction *action) {
  if (scheduler->ready_count == scheduler->ready_capacity) {
    const uint32_t capacity =
        scheduler->ready_capacity ? scheduler->ready_capacity * 2u : 64u;
    uint32_t *ready =
        (uint32_t *)realloc(scheduler->ready, sizeof(uint32_t) * capacity);
    if (!ready) {
      return false_v;
    }
    scheduler->ready = ready;
    scheduler->ready_capacity = capacity;
  }
  action->state = VKR_BAKERY_STATE_READY;
  scheduler->ready[scheduler->ready_count++] = action->index;
  return true_v;
}

/* Producers that use every core (texture encodes) still leave single-threaded
   stretches; two at once kept the cores busier, 111 to 91 s for 40 Bistro
   textures with identical bytes, and a third added nothing. */
#define VKR_BAKERY_EXCLUSIVE_SLOTS 2u

/* Picks the best admissible ready action; caller holds the mutex. */
vkr_internal VkrBakeryAction *
vkr_bakery_take_ready(VkrBakeryScheduler *scheduler) {
  VkrBakeryGraph *graph = scheduler->graph;
  const uint64_t budget = graph->config->memory_budget_mib;
  int32_t best = -1;
  for (uint32_t i = 0u; i < scheduler->ready_count; ++i) {
    const VkrBakeryAction *action = graph->actions[scheduler->ready[i]];
    if (action->exclusive_cores &&
        scheduler->exclusive_running >= VKR_BAKERY_EXCLUSIVE_SLOTS) {
      continue;
    }
    /* An action larger than the whole budget still runs, alone. */
    const bool8_t fits =
        scheduler->reserved_mib + action->est_peak_mib <= budget ||
        scheduler->running == 0u;
    if (!fits) {
      continue;
    }
    if (best < 0) {
      best = (int32_t)i;
      continue;
    }
    const VkrBakeryAction *current =
        graph->actions[scheduler->ready[(uint32_t)best]];
    if (action->priority < current->priority ||
        (action->priority == current->priority &&
         action->est_peak_mib > current->est_peak_mib)) {
      best = (int32_t)i;
    }
  }
  if (best < 0) {
    return NULL;
  }
  VkrBakeryAction *action = graph->actions[scheduler->ready[(uint32_t)best]];
  scheduler->ready[(uint32_t)best] = scheduler->ready[--scheduler->ready_count];
  return action;
}

// =============================================================================
// Task helpers (worker thread)
// =============================================================================

bool8_t vkr_bakery_task_cancelled(const VkrBakeryTask *task) {
  return vkr_atomic_bool_load(task->cancel, VKR_MEMORY_ORDER_RELAXED) ? true_v
                                                                      : false_v;
}

vkr_internal bool8_t vkr_bakery_task_is_cancelled(void *context) {
  return vkr_bakery_task_cancelled((const VkrBakeryTask *)context);
}

bool8_t vkr_bakery_task_tool(VkrBakeryTask *task, const char *tool,
                             const char *const *arguments, uint32_t count,
                             int32_t *out_exit_code) {
  VkrBakeryAction *action = task->action;
  *out_exit_code = -1;
  const char *argv[64];
  if (count + 2u > ArrayCount(argv)) {
    vkr_bakery_task_fail(task, "too many tool arguments");
    return false_v;
  }
  argv[0] = "tool";
  argv[1] = tool;
  for (uint32_t i = 0u; i < count; ++i) {
    argv[i + 2u] = arguments[i];
  }
  char usage_path[VKR_BAKERY_PATH_CAPACITY];
  (void)snprintf(usage_path, sizeof(usage_path), "%s/usage.json",
                 action->staging);
  (void)vkr_bakery_remove_file(usage_path);
  const VkrPlatformEnvironmentVariable environment[] = {
      {.name = "VKR_BAKERY_USAGE_PATH", .value = usage_path},
  };
  const VkrPlatformProcessConfig config = {
      .executable = task->config->self_path,
      .arguments = argv,
      .argument_count = count + 2u,
      .working_directory = task->config->root[0] ? task->config->root : NULL,
      .stdout_path = action->stdout_path,
      .stderr_path = action->stderr_path,
      .append_output = true_v,
      .environment = environment,
      .environment_count = ArrayCount(environment),
      .timeout_ms = VKR_BAKERY_TOOL_TIMEOUT_MS,
      .termination_grace_ms = 500u,
      .terminate_process_tree = false_v,
      .hidden = true_v,
      .is_cancelled = vkr_bakery_task_is_cancelled,
      .cancel_context = task,
  };
  bool8_t timed_out = false_v;
  const bool8_t launched =
      vkr_platform_process_run(&config, out_exit_code, &timed_out);
  /* exec reports a missing external executable as 127. */
  const bool8_t missing_executable = launched && *out_exit_code == 127 &&
                                     strcmp(tool, "exec") == 0 &&
                                     !vkr_bakery_task_cancelled(task);
  if (!launched || missing_executable) {
    vkr_bakery_task_diag(task, VKR_BAKERY_DIAG_SCHED_LAUNCH_FAILED,
                         action->display, 0u, 0u,
                         missing_executable ? arguments[0] : NULL, NULL);
    vkr_bakery_task_fail(task, "could not start `vkr_bakery tool %s`", tool);
    return false_v;
  }
  uint8_t *usage = NULL;
  uint64_t usage_length = 0u;
  if (vkr_bakery_read_file(usage_path, KB(4), &usage, &usage_length)) {
    VkrBakeryJson *value =
        vkr_bakery_json_parse(task->arena, usage, usage_length, 4u, NULL);
    int64_t cpu = 0;
    int64_t peak = 0;
    int64_t child_cpu = 0;
    int64_t child_peak = 0;
    (void)vkr_bakery_json_get_int(value, "cpu_ms", &cpu);
    (void)vkr_bakery_json_get_int(value, "peak_rss_bytes", &peak);
    (void)vkr_bakery_json_get_int(value, "children_cpu_ms", &child_cpu);
    (void)vkr_bakery_json_get_int(value, "children_peak_rss_bytes",
                                  &child_peak);
    action->cpu_ms += (uint64_t)(cpu + child_cpu);
    const uint64_t larger = (uint64_t)Max(peak, child_peak);
    if (larger > action->peak_rss_bytes) {
      action->peak_rss_bytes = larger;
      action->peak_rss_source = child_peak > peak ? "children" : "process";
    }
    free(usage);
  }
  if (timed_out) {
    vkr_bakery_task_diag(task, VKR_BAKERY_DIAG_SCHED_TIMEOUT, action->display,
                         0u, 0u, NULL, NULL);
  }
  return true_v;
}

bool8_t vkr_bakery_task_process(VkrBakeryTask *task, const char *executable,
                                const char *const *arguments, uint32_t count,
                                int32_t *out_exit_code) {
  const char *argv[62];
  if (count + 1u > ArrayCount(argv)) {
    vkr_bakery_task_fail(task, "too many process arguments");
    return false_v;
  }
  argv[0] = executable;
  for (uint32_t i = 0u; i < count; ++i) {
    argv[i + 1u] = arguments[i];
  }
  return vkr_bakery_task_tool(task, "exec", argv, count + 1u, out_exit_code);
}

const char *vkr_bakery_task_stage(VkrBakeryTask *task, const char *name) {
  char path[VKR_BAKERY_PATH_CAPACITY];
  (void)snprintf(path, sizeof(path), "%s/out/%s", task->action->staging, name);
  char directory[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  (void)vkr_bakery_make_directories(directory);
  const uint64_t length = strlen(path);
  char *copy =
      (char *)arena_alloc(task->arena, length + 1u, ARENA_MEMORY_TAG_STRING);
  if (copy) {
    MemCopy(copy, path, length + 1u);
  }
  return copy;
}

VkrBakeryProduct *vkr_bakery_action_new_product(VkrBakeryAction *action) {
  if (action->product_count == action->product_capacity) {
    const uint32_t capacity =
        action->product_capacity ? action->product_capacity * 2u : 4u;
    VkrBakeryProduct *products = (VkrBakeryProduct *)realloc(
        action->products, sizeof(VkrBakeryProduct) * capacity);
    if (!products) {
      return NULL;
    }
    action->products = products;
    action->product_capacity = capacity;
  }
  VkrBakeryProduct *product = &action->products[action->product_count++];
  MemZero(product, sizeof(*product));
  return product;
}

bool8_t vkr_bakery_task_side_product(VkrBakeryTask *task, const char *role,
                                     const char *staged_path,
                                     const char *destination) {
  VkrBakeryAction *action = task->action;
  if (!vkr_bakery_is_file(staged_path)) {
    vkr_bakery_task_diag(task, VKR_BAKERY_DIAG_SCHED_MISSING_PRODUCT,
                         action->display, 0u, 0u, role, NULL);
    vkr_bakery_task_fail(task, "no '%s' product was written", role);
    return false_v;
  }
  VkrBakeryProduct *product = vkr_bakery_action_new_product(action);
  if (!product) {
    vkr_bakery_task_fail(task, "out of memory recording products");
    return false_v;
  }
  (void)snprintf(product->role, sizeof(product->role), "%s", role);
  (void)snprintf(product->path, sizeof(product->path), "%s", staged_path);
  if (destination) {
    product->destination = strdup(destination);
    if (!product->destination) {
      vkr_bakery_task_fail(task, "out of memory recording products");
      return false_v;
    }
  }
  return true_v;
}

bool8_t vkr_bakery_task_product(VkrBakeryTask *task, const char *role,
                                const char *staged_path) {
  return vkr_bakery_task_side_product(task, role, staged_path, NULL);
}

void vkr_bakery_task_discovered(VkrBakeryTask *task, const char *path) {
  VkrBakeryAction *action = task->action;
  char absolute[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bakery_path_absolute(path, absolute, sizeof(absolute))) {
    return;
  }
  if (action->discovered_count == action->discovered_capacity) {
    const uint32_t capacity =
        action->discovered_capacity ? action->discovered_capacity * 2u : 16u;
    char **grown =
        (char **)realloc(action->discovered, sizeof(char *) * capacity);
    if (!grown) {
      return;
    }
    action->discovered = grown;
    action->discovered_capacity = capacity;
  }
  char *copy = strdup(absolute);
  if (copy) {
    action->discovered[action->discovered_count++] = copy;
  }
}

void vkr_bakery_task_progress(VkrBakeryTask *task, float64_t fraction,
                              const char *detail) {
  vkr_bakery_event_progress(task->action->id, fraction, detail);
}

void vkr_bakery_task_diag(VkrBakeryTask *task, VkrBakeryDiag diag,
                          const char *source, uint32_t line, uint32_t column,
                          const char *message, const char *hint) {
  vkr_bakery_event_diag(task->action->id, diag, source, line, column, message,
                        hint);
}

void vkr_bakery_task_fail(VkrBakeryTask *task, const char *format, ...) {
  VkrBakeryAction *action = task->action;
  if (action->failure[0]) {
    return;
  }
  va_list arguments;
  va_start(arguments, format);
  (void)vsnprintf(action->failure, sizeof(action->failure), format, arguments);
  va_end(arguments);
}

const char *vkr_bakery_task_output_tail(VkrBakeryTask *task,
                                        uint32_t max_bytes) {
  VkrBakeryBuffer tail = {0};
  const char *paths[2] = {task->action->stderr_path, task->action->stdout_path};
  for (uint32_t i = 0u; i < 2u; ++i) {
    uint8_t *data = NULL;
    uint64_t length = 0u;
    if (!vkr_bakery_read_file(paths[i], 0u, &data, &length)) {
      continue;
    }
    const uint64_t take = Min(length, (uint64_t)max_bytes / 2u);
    vkr_bakery_buffer_append(&tail, data + length - take, take);
    free(data);
  }
  const char *result = NULL;
  if (tail.data && !tail.failed) {
    char *copy = (char *)arena_alloc(task->arena, tail.length + 1u,
                                     ARENA_MEMORY_TAG_STRING);
    if (copy) {
      MemCopy(copy, tail.data, tail.length + 1u);
      result = copy;
    }
  }
  vkr_bakery_buffer_free(&tail);
  return result ? result : "";
}

vkr_internal bool8_t vkr_bakery_parse_location(char *line, char **out_file,
                                               uint32_t *out_line,
                                               uint32_t *out_column,
                                               char **out_rest) {
  /* Forms: "file:line:col: rest", "file:line: rest", "file(line): rest",
   * "file(line,col): rest". Windows drive letters keep their colon. */
  char *search = line;
  if (((line[0] >= 'A' && line[0] <= 'Z') ||
       (line[0] >= 'a' && line[0] <= 'z')) &&
      line[1] == ':') {
    search = line + 2;
  }
  char *paren = strchr(search, '(');
  char *colon = strchr(search, ':');
  if (paren && (!colon || paren < colon)) {
    char *close = strchr(paren, ')');
    if (!close || close[1] != ':') {
      return false_v;
    }
    *paren = 0;
    *out_line = (uint32_t)strtoul(paren + 1, NULL, 10);
    char *comma = strchr(paren + 1, ',');
    *out_column =
        comma && comma < close ? (uint32_t)strtoul(comma + 1, NULL, 10) : 0u;
    *out_file = line;
    *out_rest = close + 2;
    return *out_line > 0u;
  }
  if (!colon) {
    return false_v;
  }
  char *end = NULL;
  const unsigned long line_number = strtoul(colon + 1, &end, 10);
  if (end == colon + 1 || *end != ':') {
    return false_v;
  }
  *colon = 0;
  *out_file = line;
  *out_line = (uint32_t)line_number;
  *out_column = 0u;
  char *rest = end + 1;
  char *column_end = NULL;
  const unsigned long column = strtoul(rest, &column_end, 10);
  if (column_end != rest && (*column_end == ':' || *column_end == 0)) {
    *out_column = (uint32_t)column;
    rest = *column_end ? column_end + 1 : column_end;
  }
  *out_rest = rest;
  return true_v;
}

/* Reports one parsed compiler diagnostic with a root-relative path. */
vkr_internal void vkr_bakery_report_compiler_diag(
    VkrBakeryTask *task, bool8_t error, VkrBakeryDiag error_diag,
    VkrBakeryDiag warning_diag, const char *file, uint32_t line,
    uint32_t column, const char *message, const char *hint) {
  char display[VKR_BAKERY_PATH_CAPACITY];
  if (!task->config->root[0] ||
      !vkr_bakery_path_relative(task->config->root, file, display,
                                sizeof(display))) {
    (void)snprintf(display, sizeof(display), "%s", file);
  }
  vkr_bakery_task_diag(task, error ? error_diag : warning_diag, display, line,
                       column, message, hint && hint[0] ? hint : NULL);
}

/* Severity prefix of a line: 1 error, 2 warning, 0 neither. Accepts
 * "error", "fatal error", "warning" and Rust-style "error[E30015]". */
vkr_internal uint32_t vkr_bakery_severity(const char *text,
                                          const char **out_message) {
  uint32_t severity = 0u;
  const char *cursor = text;
  if (strncmp(cursor, "fatal error", 11) == 0) {
    severity = 1u;
    cursor += 11;
  } else if (strncmp(cursor, "error", 5) == 0) {
    severity = 1u;
    cursor += 5;
  } else if (strncmp(cursor, "warning", 7) == 0) {
    severity = 2u;
    cursor += 7;
  } else {
    return 0u;
  }
  if (*cursor == '[') {
    const char *close = strchr(cursor, ']');
    cursor = close ? close + 1 : cursor;
  } else if (*cursor == ' ' && cursor[1] >= '0' && cursor[1] <= '9') {
    /* Slang's "error 30015: message" form. */
    cursor += 1;
    while (*cursor >= '0' && *cursor <= '9') {
      cursor += 1;
    }
  }
  if (*cursor != ':') {
    return 0u;
  }
  cursor += 1;
  while (*cursor == ' ') {
    cursor += 1;
  }
  *out_message = cursor;
  return severity;
}

uint32_t vkr_bakery_task_parse_compiler_diags(VkrBakeryTask *task,
                                              VkrBakeryDiag error_diag,
                                              VkrBakeryDiag warning_diag) {
  uint32_t reported = 0u;
  const char *paths[2] = {task->action->stderr_path, task->action->stdout_path};
  for (uint32_t p = 0u; p < 2u; ++p) {
    uint8_t *data = NULL;
    uint64_t length = 0u;
    if (!vkr_bakery_read_file(paths[p], MB(16), &data, &length)) {
      continue;
    }
    /* A Rust-style block: severity line, then "--> file:line:col", then a
     * caret line whose trailing text is the specific message. */
    uint32_t pending = 0u;
    char pending_message[512] = {0};
    char pending_file[VKR_BAKERY_PATH_CAPACITY] = {0};
    uint32_t pending_line = 0u;
    uint32_t pending_column = 0u;
    char *cursor = (char *)data;
    while (cursor && *cursor && reported < 200u) {
      char *next = strchr(cursor, '\n');
      if (next) {
        *next = 0;
      }
      char *text = cursor;
      while (*text == ' ') {
        text += 1;
      }
      const char *message = NULL;
      const uint32_t severity = vkr_bakery_severity(text, &message);
      char *file = NULL;
      char *rest = NULL;
      uint32_t line = 0u;
      uint32_t column = 0u;
      if (severity) {
        if (pending && pending_file[0]) {
          vkr_bakery_report_compiler_diag(
              task, pending == 1u, error_diag, warning_diag, pending_file,
              pending_line, pending_column, pending_message, NULL);
          reported += 1u;
        }
        pending = severity;
        pending_file[0] = 0;
        (void)snprintf(pending_message, sizeof(pending_message), "%s", message);
      } else if (pending && strncmp(text, "--> ", 4) == 0) {
        char location[VKR_BAKERY_PATH_CAPACITY];
        (void)snprintf(location, sizeof(location), "%s", text + 4);
        if (vkr_bakery_parse_location(location, &file, &line, &column, &rest)) {
          (void)snprintf(pending_file, sizeof(pending_file), "%s", file);
          pending_line = line;
          pending_column = column;
        }
      } else if (pending && pending_file[0] && strchr(text, '^')) {
        const char *detail = strrchr(text, '^') + 1;
        while (*detail == ' ') {
          detail += 1;
        }
        /* The caret text is the specific message; the header line is a
           summary it usually repeats. */
        vkr_bakery_report_compiler_diag(
            task, pending == 1u, error_diag, warning_diag, pending_file,
            pending_line, pending_column, *detail ? detail : pending_message,
            NULL);
        reported += 1u;
        pending = 0u;
      } else if (vkr_bakery_parse_location(cursor, &file, &line, &column,
                                           &rest)) {
        while (*rest == ' ') {
          rest += 1;
        }
        const uint32_t inline_severity = vkr_bakery_severity(rest, &message);
        if (inline_severity) {
          vkr_bakery_report_compiler_diag(task, inline_severity == 1u,
                                          error_diag, warning_diag, file, line,
                                          column, message, NULL);
          reported += 1u;
        }
      }
      cursor = next ? next + 1 : NULL;
    }
    if (pending && pending_file[0]) {
      vkr_bakery_report_compiler_diag(task, pending == 1u, error_diag,
                                      warning_diag, pending_file, pending_line,
                                      pending_column, pending_message, NULL);
      reported += 1u;
    }
    free(data);
  }
  return reported;
}

// =============================================================================
// Worker
// =============================================================================

vkr_internal void vkr_bakery_run_action(VkrBakeryWorker *worker,
                                        VkrBakeryAction *action) {
  const VkrBakeryConfig *config = worker->scheduler->graph->config;
  arena_clear(worker->arena, ARENA_MEMORY_TAG_STRUCT);
  VkrBakeryTask task = {
      .config = config,
      .action = action,
      .arena = worker->arena,
      .cancel = &vkr_bakery_cancel_requested,
  };
  const float64_t started = vkr_bakery_monotonic_seconds();
  bool8_t ok = vkr_bakery_make_directories(action->staging);
  if (!ok) {
    vkr_bakery_task_fail(&task, "could not create the staging directory");
  }
  ok = ok && action->producer->run(&task);
  action->wall_ms =
      (uint64_t)((vkr_bakery_monotonic_seconds() - started) * 1000.0);
  if (vkr_bakery_task_cancelled(&task)) {
    action->status = VKR_BAKERY_ACTION_CANCELLED;
    return;
  }
  if (ok && !vkr_bakery_cache_store(config, action, worker->arena)) {
    vkr_bakery_task_diag(&task, VKR_BAKERY_DIAG_CACHE_STORE_FAILED,
                         action->display, 0u, 0u, action->failure, NULL);
    ok = false_v;
  }
  if (!ok && !action->failure[0]) {
    vkr_bakery_task_fail(&task, "%s failed", action->producer->id);
  }
  action->status = ok ? VKR_BAKERY_ACTION_OK : VKR_BAKERY_ACTION_FAILED;
}

vkr_internal void *vkr_bakery_worker_main(void *argument) {
  VkrBakeryWorker *worker = (VkrBakeryWorker *)argument;
  VkrBakeryScheduler *scheduler = worker->scheduler;
  vkr_mutex_lock(scheduler->mutex);
  for (;;) {
    if (scheduler->shutdown) {
      break;
    }
    VkrBakeryAction *action = NULL;
    if (!vkr_atomic_bool_load(&vkr_bakery_cancel_requested,
                              VKR_MEMORY_ORDER_RELAXED)) {
      action = vkr_bakery_take_ready(scheduler);
    }
    if (!action) {
      vkr_cond_wait(scheduler->wake, scheduler->mutex);
      continue;
    }
    const bool8_t exclusive = action->exclusive_cores;
    scheduler->reserved_mib += action->est_peak_mib;
    scheduler->running += 1u;
    if (exclusive) {
      scheduler->exclusive_running += 1u;
    }
    action->state = VKR_BAKERY_STATE_RUNNING;
    action->id = scheduler->graph->next_event_id++;
    action->started_seconds = vkr_bakery_monotonic_seconds();
    vkr_mutex_unlock(scheduler->mutex);

    vkr_bakery_event_start(action->id, action->prekey, action->producer->id,
                           action->display, action->label,
                           action->est_peak_mib);
    vkr_bakery_run_action(worker, action);

    vkr_mutex_lock(scheduler->mutex);
    scheduler->reserved_mib -= action->est_peak_mib;
    scheduler->running -= 1u;
    if (exclusive) {
      scheduler->exclusive_running -= 1u;
    }
    action->state = VKR_BAKERY_STATE_FINISHED;
    vkr_cond_broadcast(scheduler->wake);
  }
  vkr_mutex_unlock(scheduler->mutex);
  return NULL;
}

// =============================================================================
// Main-thread completion, publication and log tailing
// =============================================================================

/* Case-insensitive ASCII substring test over a non-terminated line. */
vkr_internal bool8_t vkr_bakery_contains_word(const char *text, size_t length,
                                              const char *word) {
  const size_t word_length = strlen(word);
  for (size_t i = 0u; i + word_length <= length; ++i) {
    size_t matched = 0u;
    while (matched < word_length &&
           (char)(text[i + matched] | 0x20) == word[matched]) {
      matched += 1u;
    }
    if (matched == word_length) {
      return true_v;
    }
  }
  return false_v;
}

vkr_internal void vkr_bakery_tail_file(VkrBakeryAction *action,
                                       const char *path, uint64_t *offset,
                                       bool8_t is_stderr, bool8_t final) {
  FILE *file = file_fopen(path, "rb");
  if (!file) {
    return;
  }
  if (fseek(file, (long)*offset, SEEK_SET) != 0) {
    fclose(file);
    return;
  }
  char chunk[VKR_BAKERY_LOG_LINE_MAX];
  size_t pending = 0u;
  for (;;) {
    const size_t read =
        fread(chunk + pending, 1u, sizeof(chunk) - 1u - pending, file);
    pending += read;
    size_t start = 0u;
    for (size_t i = 0u; i < pending; ++i) {
      if (chunk[i] != '\n') {
        continue;
      }
      size_t end = i;
      if (end > start && chunk[end - 1u] == '\r') {
        end -= 1u;
      }
      if (end > start) {
        const char *level =
            is_stderr && (vkr_bakery_contains_word(chunk + start, end - start,
                                                   "error") ||
                          vkr_bakery_contains_word(chunk + start, end - start,
                                                   "failed") ||
                          vkr_bakery_contains_word(chunk + start, end - start,
                                                   "warning"))
                ? "warn"
                : "info";
        vkr_bakery_event_log(action->id, level, chunk + start, end - start);
      }
      start = i + 1u;
    }
    *offset += start;
    if (start < pending) {
      MemCopy(chunk, chunk + start, pending - start);
    }
    pending -= start;
    if (pending == sizeof(chunk) - 1u) {
      /* An overlong line is emitted in pieces. */
      vkr_bakery_event_log(action->id, "info", chunk, pending);
      *offset += pending;
      pending = 0u;
    }
    if (read == 0u) {
      break;
    }
  }
  if (final && pending) {
    vkr_bakery_event_log(action->id, "info", chunk, pending);
    *offset += pending;
  }
  fclose(file);
}

vkr_internal void vkr_bakery_tail_action(VkrBakeryAction *action,
                                         bool8_t final) {
  vkr_bakery_tail_file(action, action->stdout_path, &action->stdout_offset,
                       false_v, final);
  vkr_bakery_tail_file(action, action->stderr_path, &action->stderr_offset,
                       true_v, final);
}

vkr_internal bool8_t vkr_bakery_publish(VkrBakeryGraph *graph,
                                        VkrBakeryAction *action) {
  for (uint32_t o = 0u; o < action->output_count; ++o) {
    const VkrBakeryOutput *output = &action->outputs[o];
    const VkrBakeryProduct *product = NULL;
    for (uint32_t p = 0u; p < action->product_count; ++p) {
      if (strcmp(action->products[p].role, output->role) == 0) {
        product = &action->products[p];
        break;
      }
    }
    if (!product) {
      vkr_bakery_event_diag(action->id, VKR_BAKERY_DIAG_SCHED_MISSING_PRODUCT,
                            action->display, 0u, 0u, output->role, NULL);
      return false_v;
    }
    char current[VKR_BAKERY_KEY_SIZE];
    if (vkr_bakery_is_file(output->path) &&
        vkr_bakery_index_hash(graph->index, output->path, current, NULL) &&
        strcmp(current, product->hash) == 0) {
      continue;
    }
    if (graph->config->dry_run) {
      continue;
    }
    if (!vkr_bakery_clone_or_copy(product->path, output->path)) {
      vkr_bakery_event_diag(action->id, VKR_BAKERY_DIAG_CACHE_PUBLISH_FAILED,
                            vkr_bakery_graph_display(graph, output->path), 0u,
                            0u, NULL, NULL);
      return false_v;
    }
    vkr_bakery_index_record(graph->index, output->path, product->hash);
  }
  for (uint32_t p = 0u; p < action->product_count; ++p) {
    const VkrBakeryProduct *product = &action->products[p];
    if (!product->destination) {
      continue;
    }
    char current[VKR_BAKERY_KEY_SIZE];
    if (vkr_bakery_is_file(product->destination) &&
        vkr_bakery_index_hash(graph->index, product->destination, current,
                              NULL) &&
        strcmp(current, product->hash) == 0) {
      continue;
    }
    if (graph->config->dry_run) {
      continue;
    }
    if (!vkr_bakery_clone_or_copy(product->path, product->destination)) {
      vkr_bakery_event_diag(
          action->id, VKR_BAKERY_DIAG_CACHE_PUBLISH_FAILED,
          vkr_bakery_graph_display(graph, product->destination), 0u, 0u, NULL,
          NULL);
      return false_v;
    }
    vkr_bakery_index_record(graph->index, product->destination, product->hash);
  }
  action->published = true_v;
  return true_v;
}

vkr_internal void vkr_bakery_emit_done(VkrBakeryAction *action) {
  const char **hashes =
      action->product_count
          ? (const char **)malloc(sizeof(char *) * action->product_count)
          : NULL;
  const uint32_t hash_count = hashes ? action->product_count : 0u;
  const uint32_t output_capacity = action->output_count + action->product_count;
  const char **outputs =
      output_capacity ? (const char **)malloc(sizeof(char *) * output_capacity)
                      : NULL;
  uint32_t output_count = 0u;
  for (uint32_t o = 0u; outputs && o < action->output_count; ++o) {
    outputs[output_count++] = action->outputs[o].path;
  }
  for (uint32_t i = 0u; i < hash_count; ++i) {
    hashes[i] = action->products[i].hash;
    if (outputs && action->products[i].destination) {
      outputs[output_count++] = action->products[i].destination;
    }
  }
  const VkrBakeryDoneEvent done = {
      .id = action->id,
      .status = action->status,
      .wall_ms = action->wall_ms,
      .cpu_ms = action->cpu_ms,
      .peak_rss_mib = action->peak_rss_bytes / (1024u * 1024u),
      .peak_rss_source =
          action->peak_rss_source ? action->peak_rss_source : "none",
      .products = hashes,
      .product_count = action->status == VKR_BAKERY_ACTION_OK ? hash_count : 0u,
      .cached = action->cached,
      .source = action->display,
      .producer = action->producer->id,
      .outputs = outputs,
      .output_count = action->published ? output_count : 0u,
  };
  vkr_bakery_event_done(&done);
  free(outputs);
  free(hashes);
}

vkr_internal void vkr_bakery_complete(VkrBakeryScheduler *scheduler,
                                      VkrBakeryAction *action);

/* Resolves a now-unblocked action: cache hit, ready, or failed. */
vkr_internal void vkr_bakery_activate(VkrBakeryScheduler *scheduler,
                                      VkrBakeryAction *action) {
  VkrBakeryGraph *graph = scheduler->graph;
  if (vkr_atomic_bool_load(&vkr_bakery_cancel_requested,
                           VKR_MEMORY_ORDER_RELAXED)) {
    action->status = VKR_BAKERY_ACTION_CANCELLED;
    vkr_bakery_complete(scheduler, action);
    return;
  }
  if (!vkr_bakery_resolve_key(graph, action)) {
    action->status = VKR_BAKERY_ACTION_FAILED;
    vkr_bakery_complete(scheduler, action);
    return;
  }
  if (vkr_bakery_cache_lookup(graph, action)) {
    action->cached = true_v;
    action->status = VKR_BAKERY_ACTION_OK;
    vkr_bakery_complete(scheduler, action);
    return;
  }
  (void)snprintf(action->staging, sizeof(action->staging), "%s/tmp/%u-%u",
                 graph->config->cache_dir, vkr_platform_get_process_id(),
                 action->index);
  (void)vkr_bakery_remove_tree(action->staging);
  (void)snprintf(action->stdout_path, sizeof(action->stdout_path),
                 "%s/stdout.log", action->staging);
  (void)snprintf(action->stderr_path, sizeof(action->stderr_path),
                 "%s/stderr.log", action->staging);
  if (graph->config->dry_run) {
    vkr_bakery_print("would run %-10s %s %s\n", action->producer->id,
                     action->display, action->label);
    action->status = VKR_BAKERY_ACTION_OK;
    action->state = VKR_BAKERY_STATE_DONE;
    scheduler->outstanding -= 1u;
    return;
  }
  (void)vkr_bakery_push_ready(scheduler, action);
}

/* Main thread, mutex held or single-threaded. Publishes, reports and
 * releases dependents. */
vkr_internal void vkr_bakery_complete(VkrBakeryScheduler *scheduler,
                                      VkrBakeryAction *action) {
  VkrBakeryGraph *graph = scheduler->graph;
  if (!action->id) {
    action->id = graph->next_event_id++;
  }
  if (action->state == VKR_BAKERY_STATE_FINISHED ||
      action->state == VKR_BAKERY_STATE_RUNNING) {
    vkr_bakery_tail_action(action, true_v);
  }
  if (action->status == VKR_BAKERY_ACTION_OK &&
      !vkr_bakery_publish(graph, action)) {
    action->status = VKR_BAKERY_ACTION_FAILED;
    (void)snprintf(action->failure, sizeof(action->failure),
                   "publication failed");
  }
  if (action->status == VKR_BAKERY_ACTION_FAILED && action->failure[0]) {
    vkr_bakery_event_log(action->id, "warn", action->failure,
                         strlen(action->failure));
  }
  vkr_bakery_emit_done(action);
  if (action->staging[0] && !graph->config->dry_run) {
    (void)vkr_bakery_remove_tree(action->staging);
  }
  action->state = VKR_BAKERY_STATE_DONE;
  scheduler->outstanding -= 1u;
  switch (action->status) {
  case VKR_BAKERY_ACTION_OK:
    scheduler->ok += 1u;
    if (action->cached) {
      scheduler->cached += 1u;
    }
    break;
  case VKR_BAKERY_ACTION_FAILED:
    if (!action->optional) {
      scheduler->failed += 1u;
    } else {
      scheduler->ok += 1u;
    }
    break;
  case VKR_BAKERY_ACTION_CANCELLED:
    scheduler->cancelled += 1u;
    break;
  }
  for (uint32_t i = 0u; i < action->dependent_count; ++i) {
    VkrBakeryAction *dependent = graph->actions[action->dependents[i]];
    if (dependent->state != VKR_BAKERY_STATE_PLANNED) {
      continue;
    }
    if (action->status != VKR_BAKERY_ACTION_OK || action->product_count == 0u) {
      dependent->status = action->status == VKR_BAKERY_ACTION_CANCELLED
                              ? VKR_BAKERY_ACTION_CANCELLED
                              : VKR_BAKERY_ACTION_FAILED;
      (void)snprintf(dependent->failure, sizeof(dependent->failure),
                     "dependency %s did not complete", action->display);
      dependent->state = VKR_BAKERY_STATE_READY; /* Leaves PLANNED once. */
      vkr_bakery_complete(scheduler, dependent);
      continue;
    }
    dependent->pending_deps -= 1u;
    if (dependent->pending_deps == 0u) {
      vkr_bakery_activate(scheduler, dependent);
    }
  }
}

vkr_internal void vkr_bakery_signal_handler(int signal_number) {
  (void)signal_number;
  vkr_atomic_bool_store(&vkr_bakery_cancel_requested, true_v,
                        VKR_MEMORY_ORDER_RELAXED);
}

void (*vkr_bakery_signal_owner)(int) = NULL;

void vkr_bakery_install_cancel_signals(void) {
  void (*handler)(int) = vkr_bakery_signal_owner ? vkr_bakery_signal_owner
                                                 : vkr_bakery_signal_handler;
  signal(SIGINT, handler);
  signal(SIGTERM, handler);
#if defined(_WIN32)
  /* A process started in its own group ignores Ctrl+C; its owner interrupts
     it with Ctrl+Break, which the C runtime raises as SIGBREAK. */
  signal(SIGBREAK, handler);
#endif
}

bool8_t vkr_bakery_graph_execute(VkrBakeryGraph *graph) {
  const float64_t started = vkr_bakery_monotonic_seconds();
  VkrBakeryScheduler scheduler = {.graph = graph};
  scheduler.outstanding = graph->action_count;
  if (!vkr_mutex_create(&graph->allocator, &scheduler.mutex) ||
      !vkr_cond_create(&graph->allocator, &scheduler.wake)) {
    return false_v;
  }
  vkr_bakery_install_cancel_signals();

  for (uint32_t i = 0u; i < graph->action_count; ++i) {
    VkrBakeryAction *action = graph->actions[i];
    if (action->pending_deps == 0u &&
        action->state == VKR_BAKERY_STATE_PLANNED) {
      vkr_bakery_activate(&scheduler, action);
    }
  }
  vkr_bakery_event_graph(graph->action_count, scheduler.cached,
                         graph->action_count - scheduler.cached,
                         graph->root_count ? graph->root_count
                                           : graph->action_count);

  uint32_t worker_count = graph->config->jobs ? graph->config->jobs : 1u;
  if (worker_count > graph->action_count) {
    worker_count = graph->action_count ? graph->action_count : 1u;
  }
  VkrBakeryWorker *workers = NULL;
  if (scheduler.outstanding > 0u && !graph->config->dry_run) {
    workers = (VkrBakeryWorker *)calloc(worker_count, sizeof(VkrBakeryWorker));
    for (uint32_t i = 0u; workers && i < worker_count; ++i) {
      workers[i].scheduler = &scheduler;
      workers[i].arena = arena_create(GB(1), MB(1));
      if (!workers[i].arena ||
          !vkr_thread_create(&graph->allocator, &workers[i].thread,
                             vkr_bakery_worker_main, &workers[i])) {
        worker_count = i;
        break;
      }
    }
    if (!workers || worker_count == 0u) {
      vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_SCHED_LAUNCH_FAILED, NULL, 0u,
                            0u, "could not start worker threads", NULL);
      free(workers);
      return false_v;
    }
    vkr_cond_broadcast(scheduler.wake);
  }

  bool8_t cancel_handled = false_v;
  while (workers && scheduler.outstanding > 0u) {
    vkr_mutex_lock(scheduler.mutex);
    const bool8_t cancel = vkr_atomic_bool_load(&vkr_bakery_cancel_requested,
                                                VKR_MEMORY_ORDER_RELAXED)
                               ? true_v
                               : false_v;
    if (cancel && !cancel_handled) {
      cancel_handled = true_v;
      while (scheduler.ready_count) {
        VkrBakeryAction *action =
            graph->actions[scheduler.ready[--scheduler.ready_count]];
        action->status = VKR_BAKERY_ACTION_CANCELLED;
        vkr_bakery_complete(&scheduler, action);
      }
    }
    bool8_t progressed = false_v;
    for (uint32_t i = 0u; i < graph->action_count; ++i) {
      VkrBakeryAction *action = graph->actions[i];
      if (action->state == VKR_BAKERY_STATE_RUNNING) {
        vkr_bakery_tail_action(action, false_v);
      } else if (action->state == VKR_BAKERY_STATE_FINISHED) {
        vkr_bakery_complete(&scheduler, action);
        progressed = true_v;
      }
    }
    if (progressed) {
      vkr_cond_broadcast(scheduler.wake);
    }
    const bool8_t stalled = scheduler.running == 0u &&
                            scheduler.ready_count == 0u &&
                            scheduler.outstanding > 0u && !progressed;
    vkr_mutex_unlock(scheduler.mutex);
    if (stalled) {
      /* Only planned actions with unmet dependencies remain: a cycle. */
      vkr_mutex_lock(scheduler.mutex);
      for (uint32_t i = 0u; i < graph->action_count; ++i) {
        VkrBakeryAction *action = graph->actions[i];
        if (action->state == VKR_BAKERY_STATE_PLANNED) {
          action->status = VKR_BAKERY_ACTION_FAILED;
          (void)snprintf(action->failure, sizeof(action->failure),
                         "dependency cycle");
          action->state = VKR_BAKERY_STATE_READY;
          vkr_bakery_complete(&scheduler, action);
        }
      }
      vkr_mutex_unlock(scheduler.mutex);
      continue;
    }
    vkr_platform_sleep(20u);
  }

  if (workers) {
    vkr_mutex_lock(scheduler.mutex);
    scheduler.shutdown = true_v;
    vkr_cond_broadcast(scheduler.wake);
    vkr_mutex_unlock(scheduler.mutex);
    for (uint32_t i = 0u; i < worker_count; ++i) {
      vkr_thread_join(workers[i].thread);
      vkr_thread_destroy(&graph->allocator, &workers[i].thread);
      arena_destroy(workers[i].arena);
    }
    free(workers);
  }
  vkr_cond_destroy(&graph->allocator, &scheduler.wake);
  vkr_mutex_destroy(&graph->allocator, &scheduler.mutex);
  free(scheduler.ready);

  vkr_bakery_event_summary(
      scheduler.ok, scheduler.failed, scheduler.cancelled, scheduler.cached,
      (uint64_t)((vkr_bakery_monotonic_seconds() - started) * 1000.0));
  return scheduler.failed == 0u && scheduler.cancelled == 0u;
}

// =============================================================================
// Probe (status and inspect)
// =============================================================================

vkr_internal bool8_t vkr_bakery_destination_current(VkrBakeryGraph *graph,
                                                    const char *path,
                                                    const char *hash) {
  char current[VKR_BAKERY_KEY_SIZE];
  return vkr_bakery_is_file(path) &&
         vkr_bakery_index_hash(graph->index, path, current, NULL) &&
         strcmp(current, hash) == 0;
}

VkrBakeryProbe vkr_bakery_graph_probe(VkrBakeryGraph *graph,
                                      VkrBakeryAction *action) {
  if (action->pending_deps) {
    return VKR_BAKERY_PROBE_PENDING;
  }
  if (!vkr_bakery_resolve_key(graph, action)) {
    return VKR_BAKERY_PROBE_ERROR;
  }
  const bool8_t force = action->force;
  action->force = false_v;
  const bool8_t cached = vkr_bakery_cache_lookup(graph, action);
  action->force = force;
  if (!cached) {
    return VKR_BAKERY_PROBE_STALE;
  }
  for (uint32_t o = 0u; o < action->output_count; ++o) {
    for (uint32_t p = 0u; p < action->product_count; ++p) {
      if (strcmp(action->products[p].role, action->outputs[o].role) == 0 &&
          !vkr_bakery_destination_current(graph, action->outputs[o].path,
                                          action->products[p].hash)) {
        return VKR_BAKERY_PROBE_OUTPUT_STALE;
      }
    }
  }
  for (uint32_t p = 0u; p < action->product_count; ++p) {
    if (action->products[p].destination &&
        !vkr_bakery_destination_current(graph, action->products[p].destination,
                                        action->products[p].hash)) {
      return VKR_BAKERY_PROBE_OUTPUT_STALE;
    }
  }
  return VKR_BAKERY_PROBE_FRESH;
}
