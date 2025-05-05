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
 *    - A `pthread_mutex_t` protects access to the event queue and the callback
 *      registry vectors.
 *    - A `pthread_cond_t` allows the worker thread to sleep efficiently when the
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

#include "array.h"
#include "core.h"
#include "defines.h"
#include "pch.h"
#include "queue.h"
#include "vector.h"

/**
 * @brief Defines the different types of events that can be processed.
 * Applications should extend this enum with their specific event types,
 * ensuring EVENT_TYPE_MAX remains the highest value.
 */
typedef enum EventType : uint16_t {
  EVENT_TYPE_NONE = 0,
  EVENT_TYPE_KEY_PRESS = 1,
  EVENT_TYPE_KEY_RELEASE = 2,
  EVENT_TYPE_MOUSE_MOVE = 3,
  EVENT_TYPE_MOUSE_CLICK = 4,
  EVENT_TYPE_MAX = 16384, /**< Maximum number of event types allowed. */
} EventType;

/**
 * @brief Represents an event to be processed.
 */
typedef struct Event {
  EventType type; /**< The type of the event, used to determine which callbacks
                   to invoke. */
  void *data; /**< Pointer to event-specific data. The ownership and lifetime
                 of this data are managed by the code that dispatches the
                 event. The event system only passes this pointer to the
                 callbacks; it does not allocate, free, or modify it. */
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

/**
 * @brief Manages the event queue, callback subscriptions, and the processing
 * thread.
 */
typedef struct EventManager {
  Arena *arena;      /**< Arena used for internal allocations (e.g., callback
                      vectors). */
  Queue_Event queue; /**< The queue holding dispatched events awaiting
                      processing. */
  Vector_EventCallback
      callbacks[EVENT_TYPE_MAX]; /**< Array of vectors, indexed by EventType,
                                    storing registered callbacks. */
  pthread_mutex_t mutex;         /**< Mutex protecting access to the queue and
                                    callback vectors. */
  pthread_cond_t cond; /**< Condition variable used by the worker thread to wait
                        for events or shutdown signal. */
  pthread_t thread;    /**< Handle for the dedicated event processing thread. */
  bool32_t running;    /**< Flag indicating if the event processor thread should
                        continue running. */
} EventManager;

/**
 * @brief Creates and initializes a new EventManager.
 * Allocates necessary resources (queue, mutex, condition variable) and starts
 * the background event processing thread.
 * @param arena The memory arena to use for internal allocations (callback
 * vectors).
 * @param manager Pointer to the EventManager structure to initialize.
 */
void event_manager_create(Arena *arena, EventManager *manager);

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
 * The caller retains ownership of the memory pointed to by `event.data` and
 * must ensure it remains valid until processed by all callbacks.
 * @param manager Pointer to the EventManager.
 * @param event The `Event` structure to dispatch. This structure is copied into
 * the queue.
 * @return `true` if the event was successfully enqueued, `false` if the queue
 * was full.
 */
bool32_t event_manager_dispatch(EventManager *manager, Event event);