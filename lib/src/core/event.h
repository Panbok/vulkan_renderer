// clang-format off

/**
 * @file event.h
 * @brief Defines a thread-safe, asynchronous event processing system.
 *
 * This system allows different parts of an application to communicate by
 * dispatching events without blocking the sender. Events are queued and processed
 * by a dedicated background thread, which invokes registered callback functions
 * for each event type.
 *
 * Key Features:
 * - **Asynchronous Processing:** Events are dispatched quickly into a queue and
 *   processed later by a worker thread, preventing the dispatcher from blocking.
 * - **Thread Safety:** Subscription, unsubscription, and dispatch operations are
 *   thread-safe using mutexes and condition variables.
 * - **Type-Based Subscription:** Callbacks are registered based on specific
 *   `EventType` values.
 * - **Dynamic Subscription:** Callbacks can be subscribed and unsubscribed at
 *   runtime.
 *
 * Architecture:
 * 1. **EventManager:** The central structure holding the event queue, callback
 *    registrations, synchronization primitives (mutex, condition variable), and
 *    the worker thread handle.
 * 2. **Event Queue:** A thread-safe queue (`Queue_Event`) stores pending events.
 * 3. **Callback Registry:** An array (`callbacks`) where each index corresponds
 *    to an `EventType`. Each element is a dynamic vector (`Vector_EventCallback`)
 *    storing the function pointers subscribed to that event type.
 * 4. **Worker Thread:** A dedicated thread (`events_processor` in event.c)
 *    continuously waits for events on the queue. When events are available, it
 *    dequeues them and executes all registered callbacks for the event's type.
 * 5. **Synchronization:**
 *    - A `Mutex` protects access to the event queue and the callback
 *      registry vectors.
 *    - A `CondVar` allows the worker thread to sleep efficiently when the
 *      queue is empty, waking up only when new events are dispatched or when the
 *      manager is shutting down.
 *
 * Usage Pattern:
 * 1. Create an `EventManager` using `event_manager_create`.
 * 2. Define callback functions matching the `EventCallback` signature.
 * 3. Register callbacks for specific event types using
 *    `event_manager_subscribe`.
 * 4. Dispatch events from any thread using `event_manager_dispatch`. The `data`
 *    pointer within the dispatched `Event` must remain valid until all callbacks
 *    have processed it (caller manages `data` lifetime).
 * 5. Optionally, unregister callbacks using `event_manager_unsubscribe`.
 * 6. When done, destroy the manager using `event_manager_destroy` to signal the
 *    worker thread, wait for it to finish, and clean up resources.
 */

// clang-format on
#pragma once

#include "containers/array.h"
#include "containers/queue.h"
#include "containers/vector.h"
#include "defines.h"
#include "event_data_buffer.h"
#include "pch.h"
#include "platform/platform.h"
#include "platform/threads.h"

// TODO: Explore possibility of re-writing this into event loop system, like
// Node.js, where events are processed in a loop, and the event manager is
// responsible for dispatching events to the event loop.

/**
 * @brief Defines the different types of events that can be processed.
 * Applications should extend this enum with their specific event types,
 * ensuring EVENT_TYPE_MAX remains the highest value.
 */
typedef enum EventType : uint16_t {
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
  EVENT_TYPE_MAX = 16384, /**< Maximum number of event types allowed. */
} EventType;

/**
 * @brief Represents an event to be processed.
 */
typedef struct Event {
  EventType type; /**< The type of the event, used to determine which callbacks
                   to invoke. */
  void *data;     /**< Pointer to event-specific data. If an event is dispatched
                     via `event_manager_dispatch` and `data_size` is greater than
                     zero, this will point to a copy of the original data, managed
                     by the event system within its arena. Otherwise, its meaning
                     and lifetime depend on how the event was created and
                     processed. */
  uint64_t data_size; /**< The size of the data pointed to by `data`, if it's
                         to be copied by `event_manager_dispatch`. If zero,
                         no data is copied, and `data` may be NULL or used
                         differently. */
} Event;

/**
 * @brief Function pointer type for event callbacks.
 * Functions matching this signature can be registered to handle specific event
 * types.
 * @param event Pointer to the `Event` structure being processed.
 * @return bool8_t Typically true, potentially used for future enhancements
 *         (e.g., stopping further propagation, though not currently
 * implemented).
 */
typedef bool8_t (*EventCallback)(Event *event);

Queue(Event);
Vector(EventCallback);
Array(EventCallback);

#define DEFAULT_EVENT_DATA_RING_BUFFER_CAPACITY                                \
  (MB(4)) // Default capacity for the event data ring buffer
#define DEFAULT_EVENT_QUEUE_CAPACITY 1024

/**
 * @brief Manages the event queue, callback subscriptions, and the processing
 * thread.
 */
typedef struct EventManager {
  Arena *arena;      /**< Arena used for internal allocations (e.g., callback
                      vectors, event data ring buffer). */
  Queue_Event queue; /**< The queue holding dispatched events awaiting
                      processing. */
  Vector_EventCallback
      callbacks[EVENT_TYPE_MAX]; /**< Array of vectors, indexed by EventType,
                                    storing registered callbacks. */

  EventDataBuffer
      event_data_buf; /**< Buffer for storing variable-sized event data. */

  Mutex mutex; /**< Mutex protecting access to the queue,
                            callback vectors, and event data buffer. */
  CondVar cond; /**< Condition variable used by the worker thread to wait
                        for events or shutdown signal. */
  Thread thread;    /**< Handle for the dedicated event processing thread. */
  bool32_t running;    /**< Flag indicating if the event processor thread should
                        continue running. */
  /*
   * IMPORTANT NOTE ON THREAD SAFETY:
   * The EventManager guarantees thread safety for its internal operations
   * (subscribing, unsubscribing, dispatching). However, the internal mutex is
   * *released* before callbacks are executed by the worker thread.
   * Therefore, it is the responsibility of the individual EventCallback
   * implementations to ensure thread safety if they access or modify any shared
   * application data that could be concurrently accessed by other threads.
   * The EventManager does *not* provide synchronization for data external to
   * its own management structures during callback execution.
   */
} EventManager;

/**
 * @brief Creates and initializes a new EventManager.
 * Allocates necessary resources (queue, mutex, condition variable) and starts
 * the background event processing thread.
 * @param manager Pointer to the EventManager structure to initialize.
 */
void event_manager_create(EventManager *manager);

/**
 * @brief Destroys an EventManager, cleans up resources, and stops the
 * processing thread.
 * Signals the processing thread to stop, waits for it to finish processing
 * remaining events and exit (joins the thread), then destroys synchronization
 * primitives and internal data structures.
 * @param manager Pointer to the EventManager to destroy.
 */
void event_manager_destroy(EventManager *manager);

/**
 * @brief Subscribes a callback function to a specific event type.
 * The provided callback will be invoked by the processing thread whenever an
 * event of the specified type is dequeued.
 * This operation is thread-safe.
 * Duplicate subscriptions of the same callback to the same event type are
 * ignored.
 * @param manager Pointer to the EventManager.
 * @param type The `EventType` to subscribe to.
 * @param callback The function pointer (`EventCallback`) to register.
 */
void event_manager_subscribe(EventManager *manager, EventType type,
                             EventCallback callback);

/**
 * @brief Unsubscribes a callback function from a specific event type.
 * Removes the specified callback from the list of subscribers for the given
 * event type. If the callback was not subscribed, this function has no effect.
 * This operation is thread-safe.
 * @param manager Pointer to the EventManager.
 * @param type The `EventType` to unsubscribe from.
 * @param callback The function pointer (`EventCallback`) to unregister.
 */
void event_manager_unsubscribe(EventManager *manager, EventType type,
                               EventCallback callback);

/**
 * @brief Dispatches an event into the queue for asynchronous processing.
 * Enqueues the event and signals the processing thread if it is waiting.
 * This operation is thread-safe and non-blocking unless the queue is full.
 * The caller provides an `Event` structure. If `event.data_size` is greater
 * than zero and `event.data` is non-NULL, the data pointed to by `event.data`
 * (up to `event.data_size` bytes) is copied into memory managed by the
 * `EventManager`'s arena. The `Event` enqueued will then point to this
 * copied data. If `event.data_size` is zero, no copy occurs.
 * @param manager Pointer to the EventManager.
 * @param event The `Event` structure to dispatch. Its `type`, `data`, and
 * `data_size` fields are used.
 * @return `true` if the event was successfully enqueued (and data copied, if
 * applicable), `false` if the queue was full or memory allocation for data
 * copy failed.
 */
bool32_t event_manager_dispatch(EventManager *manager, Event event);