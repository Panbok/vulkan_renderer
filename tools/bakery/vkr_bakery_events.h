#pragma once

#include "defines.h"
#include "memory/vkr_allocator.h"
#include "vkr_bakery_diag.h"

/* Process-wide event stream (docs/proposals/asset-build-system.md section 6).
 * Every event is one JSON line with "v":1 and a monotonic "t" in ms. With
 * --json the lines go to stdout and human lines to stderr; otherwise human
 * lines go to stdout, with a live status line when stdout is a terminal. The
 * optional log file receives every JSON line. All functions are thread-safe
 * after vkr_bakery_events_init. */

#define VKR_BAKERY_EVENT_VERSION 1u

typedef struct VkrBakeryEventsConfig {
  bool8_t json;
  bool8_t quiet;
  bool8_t verbose;
} VkrBakeryEventsConfig;

/** Receives every finished JSON event line, newline included. The daemon
 * routes a command's events to the client that requested it. Called with the
 * event mutex held, from any thread that emits events. */
typedef void (*VkrBakeryEventSink)(void *context, const char *line,
                                   uint64_t length);

bool8_t vkr_bakery_events_init(VkrAllocator *allocator,
                               const VkrBakeryEventsConfig *config);
/** Sends later event lines to `sink` as well as the configured streams. */
void vkr_bakery_events_set_sink(VkrBakeryEventSink sink, void *context);
void vkr_bakery_events_shutdown(void);
/** Starts appending JSON lines to `path`; creates its directory. */
bool8_t vkr_bakery_events_open_log(const char *path);
const char *vkr_bakery_events_log_path(void);
uint64_t vkr_bakery_events_elapsed_ms(void);
uint32_t vkr_bakery_events_error_count(void);

void vkr_bakery_event_run(const char *command, const char *platform,
                          uint32_t jobs, uint64_t memory_budget_mib,
                          const char *cache, const char *tool);
void vkr_bakery_event_graph(uint32_t actions, uint32_t cached, uint32_t pending,
                            uint32_t roots);
void vkr_bakery_event_start(uint32_t id, const char *key, const char *producer,
                            const char *source, const char *label,
                            uint64_t est_peak_mib);
/** `fraction` below zero serializes as null. */
void vkr_bakery_event_progress(uint32_t id, float64_t fraction,
                               const char *detail);
/** `id` zero omits the action. `level` is "debug", "info" or "warn". */
void vkr_bakery_event_log(uint32_t id, const char *level, const char *text,
                          uint64_t length);
void vkr_bakery_event_diag(uint32_t id, VkrBakeryDiag diag, const char *source,
                           uint32_t line, uint32_t column, const char *message,
                           const char *hint);

typedef enum VkrBakeryActionStatus {
  VKR_BAKERY_ACTION_OK = 0,
  VKR_BAKERY_ACTION_FAILED,
  VKR_BAKERY_ACTION_CANCELLED,
} VkrBakeryActionStatus;

typedef struct VkrBakeryDoneEvent {
  uint32_t id;
  VkrBakeryActionStatus status;
  uint64_t wall_ms;
  uint64_t cpu_ms;
  uint64_t peak_rss_mib;
  const char *peak_rss_source; /* "process", "children" or "none". */
  const char *const *products; /* Product hashes. */
  uint32_t product_count;
  /* Host paths the action published its products to. */
  const char *const *outputs;
  uint32_t output_count;
  bool8_t cached;
  const char *source; /* For the human line only. */
  const char *producer;
} VkrBakeryDoneEvent;

void vkr_bakery_event_done(const VkrBakeryDoneEvent *done);
void vkr_bakery_event_summary(uint32_t ok, uint32_t failed, uint32_t cancelled,
                              uint32_t cached, uint64_t wall_ms);
/** Human-only line (not part of the JSON stream), for listings and help. */
void vkr_bakery_print(const char *format, ...);
