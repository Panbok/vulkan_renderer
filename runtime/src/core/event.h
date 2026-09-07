#pragma once

#include "containers/queue.h"
#include "containers/vector.h"
#include "core/vkr_threads.h"
#include "defines.h"
#include "memory/arena.h"
#include "vkr_event_data_buffer.h"

typedef enum EventType {
  EVENT_TYPE_NONE = 0,
  EVENT_TYPE_KEY_PRESS = 1,
  EVENT_TYPE_KEY_RELEASE = 2,
  EVENT_TYPE_BUTTON_PRESS = 3,
  EVENT_TYPE_BUTTON_RELEASE = 4,
  EVENT_TYPE_MOUSE_MOVE = 5,
  EVENT_TYPE_MOUSE_WHEEL = 6,
  EVENT_TYPE_INPUT_SYSTEM_SHUTDOWN = 7,
  EVENT_TYPE_INPUT_SYSTEM_INIT = 8,
  EVENT_TYPE_WINDOW_RESIZE = 9,
  EVENT_TYPE_WINDOW_CLOSE = 10,
  EVENT_TYPE_WINDOW_INIT = 11,
  EVENT_TYPE_APPLICATION_INIT = 12,
  EVENT_TYPE_APPLICATION_SHUTDOWN = 13,
  EVENT_TYPE_APPLICATION_RESUME = 14,
  EVENT_TYPE_APPLICATION_STOP = 15,
  EVENT_TYPE_LOAD_WORLD_MESHES = 16, /**< Trigger loading of world meshes. */
  EVENT_TYPE_MAX = 16384, /**< Maximum number of event types allowed. */
} EventType;

// Dispatch copies data_size bytes; callbacks borrow the copy for execution.
// With data_size zero, callbacks receive NULL data.
typedef struct Event {
  EventType type;
  void *data;
  uint64_t data_size;
} Event;

typedef void *UserData;
// Callback return values are currently ignored.
typedef bool8_t (*EventCallback)(Event *event, UserData user_data);

typedef struct EventCallbackData {
  EventCallback callback;
  UserData user_data;
} EventCallbackData;

Queue(Event);
Vector(EventCallbackData);

#define DEFAULT_EVENT_DATA_RING_BUFFER_CAPACITY MB(4)
#define DEFAULT_EVENT_QUEUE_CAPACITY 1024

typedef struct EventManager {
  Arena *arena;
  Arena *callback_arena; // Worker scratch; released after join.
  VkrAllocator allocator;
  Queue_Event queue;
  Vector_EventCallbackData callbacks[EVENT_TYPE_MAX];
  VkrEventDataBuffer event_data_buf;
  VkrMutex
      mutex; // Protects the queue, subscriptions, payload buffer, and running.
  VkrCondVar cond;
  VkrThread thread;
  bool32_t running;
} EventManager;

// Create starts one worker or returns false with all resources released.
// Destroy drains queued events and joins a successfully created manager.
bool8_t event_manager_create(EventManager *manager);
void event_manager_destroy(EventManager *manager);

// Return false for invalid input or allocation failure; existing subscriptions
// remain intact. Duplicate (callback, user_data) pairs succeed. Callbacks
// execute outside the manager mutex and must synchronize access to shared
// application data.
VKR_MUST_USE bool8_t event_manager_subscribe(EventManager *manager,
                                             EventType type,
                                             EventCallback callback,
                                             UserData user_data);

// Remove the first registration matching callback. An already copied worker
// snapshot may still invoke it; unsubscribe does not wait for callback
// completion.
void event_manager_unsubscribe(EventManager *manager, EventType type,
                               EventCallback callback);

// Serialize enqueue/payload copy with the manager mutex. Return false when
// input is invalid, the queue is full, or contiguous payload storage is
// exhausted.
bool32_t event_manager_dispatch(EventManager *manager, Event event);
