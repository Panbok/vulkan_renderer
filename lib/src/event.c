#include "event.h"
#include "arena.h"
#include "defines.h"

/**
 * @brief The main function for the dedicated event processing thread.
 * Waits for events on the queue using a condition variable. When woken up,
 * it checks if the manager is still running and if events are available.
 * If an event is dequeued, it retrieves the list of subscribed callbacks
 * for that event type (copying them to a temporary buffer to minimize lock
 * duration), releases the lock, and invokes the callbacks.
 * Uses a thread-local arena with scratch allocations for the temporary callback
 * buffer.
 * Continues processing until the `manager->running` flag is set to false and
 * the queue is empty.
 * Cleans up the thread-local arena before exiting.
 * @param arg Pointer to the EventManager instance.
 * @return void* Always returns NULL.
 */
static void *events_processor(void *arg) {
  EventManager *manager = (EventManager *)arg;
  Arena *local_thread_arena = arena_create(KB(4), KB(4));
  bool32_t should_run = true;

  while (should_run) {
    Event event;
    bool32_t event_dequeued = false;

    pthread_mutex_lock(&manager->mutex);

    while (queue_is_empty_Event(&manager->queue) && manager->running) {
      pthread_cond_wait(&manager->cond, &manager->mutex);
    }

    should_run = manager->running;

    if (!queue_is_empty_Event(&manager->queue)) {
      event_dequeued = queue_dequeue_Event(&manager->queue, &event);
    }

    pthread_mutex_unlock(&manager->mutex);

    if (event_dequeued) {
      if (event.type < EVENT_TYPE_MAX) {
        pthread_mutex_lock(&manager->mutex);
        Scratch scratch = scratch_create(local_thread_arena);
        Vector_EventCallback *callbacks = &manager->callbacks[event.type];
        uint16_t subs_count = callbacks->length;
        Array_EventCallback local_callbacks =
            array_create_EventCallback(scratch.arena, subs_count);
        if (subs_count > 0) {
          MemCopy(local_callbacks.data, callbacks->data,
                  subs_count * sizeof(EventCallback));
        }
        pthread_mutex_unlock(&manager->mutex);

        for (uint16_t i = 0; i < subs_count; i++) {
          if (local_callbacks.data[i] != NULL) {
            local_callbacks.data[i](&event);
          }
        }

        array_destroy_EventCallback(&local_callbacks);
        scratch_destroy(scratch);
      } else {
        log_warn("Processed event with invalid type: %u", event.type);
      }
    }
  }

  arena_destroy(local_thread_arena);
  return NULL;
}

void event_manager_create(Arena *arena, EventManager *manager) {
  assert_log(arena != NULL, "Arena is NULL");
  assert_log(manager != NULL, "Manager is NULL");

  manager->arena = arena;
  MemZero(manager->callbacks, sizeof(manager->callbacks));
  manager->queue = queue_create_Event(arena, 1024);

  pthread_mutex_init(&manager->mutex, NULL);
  pthread_cond_init(&manager->cond, NULL);
  manager->running = true;

  pthread_create(&manager->thread, NULL, events_processor, manager);
}

void event_manager_destroy(EventManager *manager) {
  assert_log(manager != NULL, "Manager is NULL");
  pthread_mutex_lock(&manager->mutex);
  manager->running = false;
  pthread_mutex_unlock(&manager->mutex);
  pthread_cond_signal(&manager->cond);

  pthread_join(manager->thread, NULL);
  pthread_mutex_destroy(&manager->mutex);
  pthread_cond_destroy(&manager->cond);

  for (uint16_t i = 0; i < EVENT_TYPE_MAX; i++) {
    vector_destroy_EventCallback(&manager->callbacks[i]);
  }
}

void event_manager_subscribe(EventManager *manager, EventType type,
                             EventCallback callback) {
  assert_log(type < EVENT_TYPE_MAX, "Invalid event type");
  assert_log(callback != NULL, "Callback is NULL");
  assert_log(manager != NULL, "Manager is NULL");

  pthread_mutex_lock(&manager->mutex);

  if (manager->callbacks[type].data == NULL) {
    manager->callbacks[type] = vector_create_EventCallback(manager->arena);
  }

  uint16_t subs_count = manager->callbacks[type].length;
  for (uint16_t i = 0; i < subs_count; i++) {
    if (manager->callbacks[type].data[i] == callback) {
      log_warn("Callback already subscribed");
      pthread_mutex_unlock(&manager->mutex);
      return;
    }
  }

  vector_push_EventCallback(&manager->callbacks[type], callback);
  pthread_mutex_unlock(&manager->mutex);
}

void event_manager_unsubscribe(EventManager *manager, EventType type,
                               EventCallback callback) {
  assert_log(type < EVENT_TYPE_MAX, "Invalid event type");
  assert_log(callback != NULL, "Callback is NULL");
  assert_log(manager != NULL, "Manager is NULL");

  pthread_mutex_lock(&manager->mutex);
  VectorFindResult res =
      vector_find_EventCallback(&manager->callbacks[type], &callback);
  if (res.found) {
    vector_pop_at_EventCallback(&manager->callbacks[type], res.index, NULL);
  }
  pthread_mutex_unlock(&manager->mutex);
}

bool32_t event_manager_dispatch(EventManager *manager, Event event) {
  assert_log(event.type < EVENT_TYPE_MAX, "Invalid event type");
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(event.data != NULL, "Event data is NULL");

  pthread_mutex_lock(&manager->mutex);
  if (!queue_enqueue_Event(&manager->queue, event)) {
    log_warn("Failed to enqueue event");
    pthread_mutex_unlock(&manager->mutex);
    return false;
  }

  pthread_cond_signal(&manager->cond);
  pthread_mutex_unlock(&manager->mutex);

  return true;
}
