#pragma once

#include "containers/bitset.h"
#include "core/event.h"
#include "core/vkr_clock.h"
#include "core/vkr_gamepad.h"
#include "core/vkr_threads.h"
#include "core/vkr_window.h"
#include "defines.h"
#include "memory/vkr_allocator.h"

typedef enum VkrApplicationHostFlag {
  VKR_APPLICATION_HOST_FLAG_NONE = 0,
  VKR_APPLICATION_HOST_FLAG_INITIALIZED = 1 << 0,
  VKR_APPLICATION_HOST_FLAG_RUNNING = 1 << 1,
  VKR_APPLICATION_HOST_FLAG_SUSPENDED = 1 << 2,
  VKR_APPLICATION_HOST_FLAG_INIT_DISPATCHED = 1 << 3,
} VkrApplicationHostFlag;

typedef struct VkrApplicationHostConfig {
  const char *title;
  int32_t x;
  int32_t y;
  uint32_t width;
  uint32_t height;
  uint64_t target_frame_rate;
  bool8_t windowed;
  bool8_t window_hidden;
  float64_t fixed_delta_seconds;
} VkrApplicationHostConfig;

typedef struct VkrApplicationHostFrame {
  float64_t delta_seconds;
  float64_t elapsed_seconds;
  float64_t absolute_time_seconds;
} VkrApplicationHostFrame;

typedef struct VkrApplicationHostCallbacks {
  void *state;
  /** Return false to end the current run without another frame callback. */
  bool8_t (*frame)(void *state, const VkrApplicationHostFrame *frame);
  /** Called when the host has selected a limiter sleep interval. */
  void (*sleep)(void *state, uint64_t milliseconds);
  /** Called after the optional limiter sleep and before input is advanced. */
  void (*frame_complete)(void *state, const VkrApplicationHostFrame *frame);
  /**
   * Application init, stop, resume, and shutdown arrive on the lifecycle
   * caller's thread. Window and input events arrive on the event worker and
   * require synchronization with frame and lifecycle callbacks. The payload is
   * borrowed for this callback only.
   */
  bool8_t (*event)(void *state, Event *event);
} VkrApplicationHostCallbacks;

/* Keep the host and its borrowed allocator at stable addresses through
 * destroy; window callbacks and synchronization objects refer to them.
 * Callback state remains caller-owned through destroy, which drains queued
 * window and input callbacks before joining the event worker. */
typedef struct VkrApplicationHost {
  VkrApplicationHostConfig config;
  VkrAllocator *allocator;
  EventManager events;
  VkrWindow window;
  VkrClock clock;
  float64_t last_frame_time;
  Bitset8 flags;
  VkrMutex mutex;
  VkrGamepad gamepad;
  VkrApplicationHostCallbacks callbacks;
} VkrApplicationHost;

/**
 * Validates its arguments and initializes platform, window, input and event
 * state. It does not dispatch APPLICATION_INIT.
 */
bool8_t vkr_application_host_create(VkrApplicationHost *host,
                                    const VkrApplicationHostConfig *config,
                                    VkrAllocator *allocator);
void vkr_application_host_set_callbacks(
    VkrApplicationHost *host, const VkrApplicationHostCallbacks *callbacks);
/**
 * Dispatches APPLICATION_INIT once, immediately before the first run loop.
 * Lifecycle callbacks execute synchronously. If INIT suspends or closes the
 * host, returns before the first frame callback. The caller may resume and run
 * it again; INIT is not repeated. The event manager still receives each
 * lifecycle event for independently subscribed asynchronous observers.
 */
void vkr_application_host_run(VkrApplicationHost *host);
void vkr_application_host_stop(VkrApplicationHost *host);
void vkr_application_host_resume(VkrApplicationHost *host);
void vkr_application_host_close(VkrApplicationHost *host);
/**
 * Synchronously dispatches APPLICATION_SHUTDOWN once only after
 * APPLICATION_INIT, before the owner destroys resources that callbacks may
 * inspect. Independently subscribed event-manager observers remain async.
 */
void vkr_application_host_shutdown(VkrApplicationHost *host);
void vkr_application_host_destroy(VkrApplicationHost *host);

vkr_internal INLINE bool8_t
vkr_application_host_is_windowed(const VkrApplicationHost *host) {
  return host->config.windowed;
}
