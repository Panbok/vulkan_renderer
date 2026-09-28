#include "vkr_bakery_events.h"
#include "filesystem/filesystem.h"

#include "core/vkr_threads.h"
#include "vkr_bakery_buffer.h"
#include "vkr_bakery_json.h"
#include "vkr_bakery_os.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define vkr_bakery_isatty(fd) _isatty(fd)
#else
#include <unistd.h>
#define vkr_bakery_isatty(fd) isatty(fd)
#endif

#define VKR_BAKERY_STATUS_SLOTS 16u

typedef struct VkrBakeryStatusSlot {
  uint32_t id;
  float64_t fraction;
  char label[96];
} VkrBakeryStatusSlot;

typedef struct VkrBakeryEventState {
  bool8_t initialized;
  VkrBakeryEventsConfig config;
  VkrMutex mutex;
  float64_t start_seconds;
  FILE *log;
  char log_path[VKR_BAKERY_PATH_CAPACITY];
  bool8_t status_line; /* stdout is a terminal and human output is there. */
  bool8_t status_visible;
  uint32_t errors;
  uint32_t total_actions;
  uint32_t finished_actions;
  VkrBakeryStatusSlot running[VKR_BAKERY_STATUS_SLOTS];
  VkrBakeryBuffer line;
  VkrBakeryEventSink sink;
  void *sink_context;
} VkrBakeryEventState;

vkr_internal VkrBakeryEventState vkr_bakery_events = {0};

bool8_t vkr_bakery_events_init(VkrAllocator *allocator,
                               const VkrBakeryEventsConfig *config) {
  VkrBakeryEventState *state = &vkr_bakery_events;
  MemZero(state, sizeof(*state));
  state->config = *config;
  if (!vkr_mutex_create(allocator, &state->mutex)) {
    return false_v;
  }
  state->start_seconds = vkr_bakery_monotonic_seconds();
  state->status_line = !config->json && !config->quiet && vkr_bakery_isatty(1)
                           ? true_v
                           : false_v;
  state->initialized = true_v;
  return true_v;
}

void vkr_bakery_events_set_sink(VkrBakeryEventSink sink, void *context) {
  VkrBakeryEventState *state = &vkr_bakery_events;
  if (state->initialized) {
    vkr_mutex_lock(state->mutex);
  }
  state->sink = sink;
  state->sink_context = context;
  if (state->initialized) {
    vkr_mutex_unlock(state->mutex);
  }
}

void vkr_bakery_events_shutdown(void) {
  VkrBakeryEventState *state = &vkr_bakery_events;
  if (!state->initialized) {
    return;
  }
  if (state->log) {
    fclose(state->log);
  }
  vkr_bakery_buffer_free(&state->line);
  state->initialized = false_v;
}

bool8_t vkr_bakery_events_open_log(const char *path) {
  VkrBakeryEventState *state = &vkr_bakery_events;
  char directory[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  if (directory[0] && !vkr_bakery_make_directories(directory)) {
    return false_v;
  }
  FILE *log = file_fopen(path, "ab");
  if (!log) {
    return false_v;
  }
  vkr_mutex_lock(state->mutex);
  if (state->log) {
    fclose(state->log);
  }
  state->log = log;
  (void)snprintf(state->log_path, sizeof(state->log_path), "%s", path);
  vkr_mutex_unlock(state->mutex);
  return true_v;
}

const char *vkr_bakery_events_log_path(void) {
  return vkr_bakery_events.log_path[0] ? vkr_bakery_events.log_path : NULL;
}

uint64_t vkr_bakery_events_elapsed_ms(void) {
  const float64_t elapsed =
      vkr_bakery_monotonic_seconds() - vkr_bakery_events.start_seconds;
  return elapsed > 0.0 ? (uint64_t)(elapsed * 1000.0) : 0u;
}

uint32_t vkr_bakery_events_error_count(void) {
  return vkr_bakery_events.errors;
}

// =============================================================================
// Line assembly (callers hold the mutex)
// =============================================================================

vkr_internal void vkr_bakery_line_begin(VkrBakeryBuffer *line,
                                        const char *event) {
  vkr_bakery_buffer_reset(line);
  vkr_bakery_buffer_appendf(
      line, "{\"v\":%u,\"t\":%llu,\"ev\":\"%s\"", VKR_BAKERY_EVENT_VERSION,
      (unsigned long long)vkr_bakery_events_elapsed_ms(), event);
}

vkr_internal void vkr_bakery_line_string(VkrBakeryBuffer *line,
                                         const char *name, const char *value) {
  vkr_bakery_buffer_appendf(line, ",\"%s\":", name);
  if (!value) {
    vkr_bakery_buffer_append(line, "null", 4u);
    return;
  }
  vkr_bakery_json_write_string(
      line, (String8){.str = (uint8_t *)value, .length = strlen(value)});
}

vkr_internal void vkr_bakery_line_u64(VkrBakeryBuffer *line, const char *name,
                                      uint64_t value) {
  vkr_bakery_buffer_appendf(line, ",\"%s\":%llu", name,
                            (unsigned long long)value);
}

/* Writes the finished JSON line to the stream and the log. */
vkr_internal void vkr_bakery_line_emit(VkrBakeryEventState *state) {
  vkr_bakery_buffer_append(&state->line, "}\n", 2u);
  if (state->line.failed) {
    return;
  }
  if (state->config.json) {
    fwrite(state->line.data, 1u, (size_t)state->line.length, stdout);
    fflush(stdout);
  }
  if (state->log) {
    fwrite(state->line.data, 1u, (size_t)state->line.length, state->log);
    fflush(state->log);
  }
  if (state->sink) {
    state->sink(state->sink_context, (const char *)state->line.data,
                state->line.length);
  }
}

// =============================================================================
// Human output (callers hold the mutex)
// =============================================================================

vkr_internal FILE *vkr_bakery_human_stream(const VkrBakeryEventState *state) {
  return state->config.json ? stderr : stdout;
}

vkr_internal void vkr_bakery_status_clear(VkrBakeryEventState *state) {
  if (state->status_visible) {
    fputs("\r\033[K", stdout);
    state->status_visible = false_v;
  }
}

vkr_internal void vkr_bakery_status_draw(VkrBakeryEventState *state) {
  if (!state->status_line) {
    return;
  }
  char text[256];
  int length = snprintf(text, sizeof(text), "[%u/%u]", state->finished_actions,
                        state->total_actions);
  uint32_t shown = 0u;
  for (uint32_t i = 0u; i < VKR_BAKERY_STATUS_SLOTS && length > 0 &&
                        (uint32_t)length < sizeof(text) - 40u;
       ++i) {
    const VkrBakeryStatusSlot *slot = &state->running[i];
    if (!slot->id) {
      continue;
    }
    if (shown == 3u) {
      length += snprintf(text + length, sizeof(text) - (size_t)length, " ...");
      break;
    }
    if (slot->fraction >= 0.0) {
      length +=
          snprintf(text + length, sizeof(text) - (size_t)length, " %.40s %u%%",
                   slot->label, (uint32_t)(slot->fraction * 100.0));
    } else {
      length += snprintf(text + length, sizeof(text) - (size_t)length, " %.40s",
                         slot->label);
    }
    shown += 1u;
  }
  fputs("\r\033[K", stdout);
  fputs(text, stdout);
  fflush(stdout);
  state->status_visible = true_v;
}

vkr_internal void vkr_bakery_human(VkrBakeryEventState *state,
                                   const char *format, ...) {
  if (state->config.quiet) {
    return;
  }
  vkr_bakery_status_clear(state);
  FILE *stream = vkr_bakery_human_stream(state);
  va_list arguments;
  va_start(arguments, format);
  vfprintf(stream, format, arguments);
  va_end(arguments);
  fputc('\n', stream);
  fflush(stream);
  vkr_bakery_status_draw(state);
}

vkr_internal VkrBakeryStatusSlot *
vkr_bakery_status_slot(VkrBakeryEventState *state, uint32_t id) {
  for (uint32_t i = 0u; i < VKR_BAKERY_STATUS_SLOTS; ++i) {
    if (state->running[i].id == id) {
      return &state->running[i];
    }
  }
  return NULL;
}

// =============================================================================
// Events
// =============================================================================

void vkr_bakery_event_run(const char *command, const char *platform,
                          uint32_t jobs, uint64_t memory_budget_mib,
                          const char *cache, const char *tool) {
  VkrBakeryEventState *state = &vkr_bakery_events;
  vkr_mutex_lock(state->mutex);
  vkr_bakery_line_begin(&state->line, "run");
  vkr_bakery_line_string(&state->line, "command", command);
  vkr_bakery_line_string(&state->line, "platform", platform);
  vkr_bakery_line_u64(&state->line, "jobs", jobs);
  vkr_bakery_line_u64(&state->line, "memory_budget_mib", memory_budget_mib);
  vkr_bakery_line_string(&state->line, "cache", cache);
  vkr_bakery_line_string(&state->line, "tool", tool);
  vkr_bakery_line_emit(state);
  if (state->config.verbose) {
    vkr_bakery_human(state, "vkr_bakery %s: %u jobs, %llu MiB budget, cache %s",
                     command, jobs, (unsigned long long)memory_budget_mib,
                     cache ? cache : "(none)");
  }
  vkr_mutex_unlock(state->mutex);
}

void vkr_bakery_event_graph(uint32_t actions, uint32_t cached, uint32_t pending,
                            uint32_t roots) {
  VkrBakeryEventState *state = &vkr_bakery_events;
  vkr_mutex_lock(state->mutex);
  state->total_actions = actions;
  state->finished_actions = cached;
  vkr_bakery_line_begin(&state->line, "graph");
  vkr_bakery_line_u64(&state->line, "actions", actions);
  vkr_bakery_line_u64(&state->line, "cached", cached);
  vkr_bakery_line_u64(&state->line, "pending", pending);
  vkr_bakery_line_u64(&state->line, "roots", roots);
  vkr_bakery_line_emit(state);
  vkr_bakery_human(state, "%u actions: %u cached, %u to run", actions, cached,
                   pending);
  vkr_mutex_unlock(state->mutex);
}

void vkr_bakery_event_start(uint32_t id, const char *key, const char *producer,
                            const char *source, const char *label,
                            uint64_t est_peak_mib) {
  VkrBakeryEventState *state = &vkr_bakery_events;
  vkr_mutex_lock(state->mutex);
  vkr_bakery_line_begin(&state->line, "start");
  vkr_bakery_line_u64(&state->line, "id", id);
  vkr_bakery_line_string(&state->line, "key", key);
  vkr_bakery_line_string(&state->line, "producer", producer);
  vkr_bakery_line_string(&state->line, "source", source);
  vkr_bakery_line_string(&state->line, "label", label);
  vkr_bakery_line_u64(&state->line, "est_peak_mib", est_peak_mib);
  vkr_bakery_line_emit(state);
  VkrBakeryStatusSlot *slot = vkr_bakery_status_slot(state, 0u);
  if (slot) {
    slot->id = id;
    slot->fraction = -1.0;
    (void)snprintf(slot->label, sizeof(slot->label), "%s",
                   vkr_bakery_path_name(source ? source : producer));
  }
  if (state->config.verbose || !state->status_line) {
    vkr_bakery_human(state, "start  %-10s %s%s%s", producer,
                     source ? source : "", label && label[0] ? "  " : "",
                     label ? label : "");
  } else {
    vkr_bakery_status_draw(state);
  }
  vkr_mutex_unlock(state->mutex);
}

void vkr_bakery_event_progress(uint32_t id, float64_t fraction,
                               const char *detail) {
  VkrBakeryEventState *state = &vkr_bakery_events;
  vkr_mutex_lock(state->mutex);
  vkr_bakery_line_begin(&state->line, "progress");
  vkr_bakery_line_u64(&state->line, "id", id);
  if (fraction >= 0.0) {
    vkr_bakery_buffer_appendf(&state->line, ",\"fraction\":%.4f",
                              fraction > 1.0 ? 1.0 : fraction);
  } else {
    vkr_bakery_buffer_append_cstr(&state->line, ",\"fraction\":null");
  }
  vkr_bakery_line_string(&state->line, "detail", detail ? detail : "");
  vkr_bakery_line_emit(state);
  VkrBakeryStatusSlot *slot = vkr_bakery_status_slot(state, id);
  if (slot) {
    slot->fraction = fraction;
  }
  vkr_bakery_status_draw(state);
  vkr_mutex_unlock(state->mutex);
}

void vkr_bakery_event_log(uint32_t id, const char *level, const char *text,
                          uint64_t length) {
  VkrBakeryEventState *state = &vkr_bakery_events;
  vkr_mutex_lock(state->mutex);
  vkr_bakery_line_begin(&state->line, "log");
  if (id) {
    vkr_bakery_line_u64(&state->line, "id", id);
  }
  vkr_bakery_line_string(&state->line, "level", level);
  vkr_bakery_buffer_append_cstr(&state->line, ",\"text\":");
  vkr_bakery_json_write_string(
      &state->line, (String8){.str = (uint8_t *)text, .length = length});
  vkr_bakery_line_emit(state);
  const bool8_t warn = strcmp(level, "warn") == 0;
  if (state->config.verbose || warn) {
    vkr_bakery_human(state, "%s%.*s", warn ? "warn   " : "       ", (int)length,
                     text);
  }
  vkr_mutex_unlock(state->mutex);
}

void vkr_bakery_event_diag(uint32_t id, VkrBakeryDiag diag, const char *source,
                           uint32_t line, uint32_t column, const char *message,
                           const char *hint) {
  VkrBakeryEventState *state = &vkr_bakery_events;
  const VkrBakeryDiagInfo *info = vkr_bakery_diag_info(diag);
  const bool8_t error = info->severity == VKR_BAKERY_SEVERITY_ERROR;
  vkr_mutex_lock(state->mutex);
  if (error) {
    state->errors += 1u;
  }
  vkr_bakery_line_begin(&state->line, "diag");
  if (id) {
    vkr_bakery_line_u64(&state->line, "id", id);
  }
  vkr_bakery_line_string(&state->line, "code", info->code);
  vkr_bakery_line_string(&state->line, "severity", error ? "error" : "warning");
  vkr_bakery_line_string(&state->line, "source", source);
  vkr_bakery_line_u64(&state->line, "line", line);
  vkr_bakery_line_u64(&state->line, "column", column);
  vkr_bakery_line_string(&state->line, "message",
                         message ? message : info->description);
  vkr_bakery_line_string(&state->line, "hint", hint);
  vkr_bakery_line_emit(state);
  if (source && line) {
    vkr_bakery_human(state, "%s %s: %s:%u:%u: %s%s%s",
                     error ? "error" : "warning", info->code, source, line,
                     column, message ? message : info->description,
                     hint ? "\n       hint: " : "", hint ? hint : "");
  } else {
    vkr_bakery_human(state, "%s %s: %s%s%s%s%s", error ? "error" : "warning",
                     info->code, source ? source : "", source ? ": " : "",
                     message ? message : info->description,
                     hint ? "\n       hint: " : "", hint ? hint : "");
  }
  vkr_mutex_unlock(state->mutex);
}

void vkr_bakery_event_done(const VkrBakeryDoneEvent *done) {
  static const char *const statuses[] = {"ok", "failed", "cancelled"};
  VkrBakeryEventState *state = &vkr_bakery_events;
  vkr_mutex_lock(state->mutex);
  state->finished_actions += 1u;
  vkr_bakery_line_begin(&state->line, "done");
  vkr_bakery_line_u64(&state->line, "id", done->id);
  vkr_bakery_line_string(&state->line, "status", statuses[done->status]);
  vkr_bakery_line_u64(&state->line, "wall_ms", done->wall_ms);
  vkr_bakery_line_u64(&state->line, "cpu_ms", done->cpu_ms);
  vkr_bakery_line_u64(&state->line, "peak_rss_mib", done->peak_rss_mib);
  vkr_bakery_line_string(&state->line, "peak_rss_source",
                         done->peak_rss_source ? done->peak_rss_source
                                               : "none");
  vkr_bakery_buffer_append_cstr(&state->line, ",\"products\":[");
  for (uint32_t i = 0u; i < done->product_count; ++i) {
    vkr_bakery_buffer_appendf(&state->line, "%s\"%s\"", i ? "," : "",
                              done->products[i]);
  }
  vkr_bakery_buffer_append_cstr(&state->line, "],\"outputs\":[");
  for (uint32_t i = 0u; i < done->output_count; ++i) {
    if (i) {
      vkr_bakery_buffer_append(&state->line, ",", 1u);
    }
    vkr_bakery_json_write_string(&state->line,
                                 (String8){.str = (uint8_t *)done->outputs[i],
                                           .length = strlen(done->outputs[i])});
  }
  vkr_bakery_buffer_appendf(&state->line, "],\"cached\":%s",
                            done->cached ? "true" : "false");
  vkr_bakery_line_emit(state);
  VkrBakeryStatusSlot *slot = vkr_bakery_status_slot(state, done->id);
  if (slot) {
    MemZero(slot, sizeof(*slot));
  }
  if (done->status != VKR_BAKERY_ACTION_OK || !state->config.quiet) {
    vkr_bakery_human(
        state, "%-9s %-10s %s  %.2fs cpu %.2fs %llu MiB",
        done->status == VKR_BAKERY_ACTION_OK
            ? (done->cached ? "cached" : "done")
            : statuses[done->status],
        done->producer ? done->producer : "", done->source ? done->source : "",
        (float64_t)done->wall_ms / 1000.0, (float64_t)done->cpu_ms / 1000.0,
        (unsigned long long)done->peak_rss_mib);
  }
  vkr_mutex_unlock(state->mutex);
}

void vkr_bakery_event_summary(uint32_t ok, uint32_t failed, uint32_t cancelled,
                              uint32_t cached, uint64_t wall_ms) {
  VkrBakeryEventState *state = &vkr_bakery_events;
  vkr_mutex_lock(state->mutex);
  vkr_bakery_line_begin(&state->line, "summary");
  vkr_bakery_line_u64(&state->line, "ok", ok);
  vkr_bakery_line_u64(&state->line, "failed", failed);
  vkr_bakery_line_u64(&state->line, "cancelled", cancelled);
  vkr_bakery_line_u64(&state->line, "cached", cached);
  vkr_bakery_line_u64(&state->line, "wall_ms", wall_ms);
  vkr_bakery_line_string(&state->line, "log",
                         state->log_path[0] ? state->log_path : NULL);
  vkr_bakery_line_emit(state);
  vkr_bakery_status_clear(state);
  vkr_bakery_human(state,
                   "%u ok, %u failed, %u cancelled, %u cached in %.2fs%s%s", ok,
                   failed, cancelled, cached, (float64_t)wall_ms / 1000.0,
                   state->log_path[0] ? "; log " : "", state->log_path);
  vkr_mutex_unlock(state->mutex);
}

void vkr_bakery_print(const char *format, ...) {
  VkrBakeryEventState *state = &vkr_bakery_events;
  if (state->initialized) {
    vkr_mutex_lock(state->mutex);
    vkr_bakery_status_clear(state);
  }
  FILE *stream = state->initialized && state->config.json ? stderr : stdout;
  va_list arguments;
  va_start(arguments, format);
  vfprintf(stream, format, arguments);
  va_end(arguments);
  fflush(stream);
  if (state->initialized) {
    vkr_mutex_unlock(state->mutex);
  }
}
