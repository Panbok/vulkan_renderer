#include "event.h"

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
      if (event_dequeued && event.data_size > 0 && event.data != NULL) {
        if (!event_data_buffer_free(&manager->event_data_buf,
                                    event.data_size)) {
          log_error("Events_processor: Failed to free data for event type %d, "
                    "size %llu from event data buffer.",
                    event.type, event.data_size);
        }
      }
    }

    if (!event_dequeued) {
      pthread_mutex_unlock(&manager->mutex);
      continue;
    }

    if (event.type >= EVENT_TYPE_MAX) {
      log_warn("Processed event with invalid type: %u", event.type);
      pthread_mutex_unlock(&manager->mutex);
      continue;
    }

    Scratch scratch = scratch_create(local_thread_arena);
    Vector_EventCallback *callbacks_vec = &manager->callbacks[event.type];
    uint16_t subs_count = callbacks_vec->length;

    if (subs_count == 0) {
      scratch_destroy(scratch, ARENA_MEMORY_TAG_VECTOR);
      pthread_mutex_unlock(&manager->mutex);
      continue;
    }

    Array_EventCallback local_callbacks_copy =
        array_create_EventCallback(scratch.arena, subs_count);
    if (callbacks_vec->data != NULL && local_callbacks_copy.data != NULL) {
      MemCopy(local_callbacks_copy.data, callbacks_vec->data,
              subs_count * sizeof(EventCallback));
      pthread_mutex_unlock(&manager->mutex);

      for (uint16_t i = 0; i < subs_count; i++) {
        if (local_callbacks_copy.data[i] != NULL) {
          local_callbacks_copy.data[i](&event);
        }
      }

    } else {
      log_warn("Event_processor: subs_count (%u) for event type %d, but "
               "vector or array data pointer is NULL. Callbacks skipped.",
               subs_count, event.type);
      pthread_mutex_unlock(&manager->mutex);
    }

    array_destroy_EventCallback(&local_callbacks_copy);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_VECTOR);
  }

  arena_destroy(local_thread_arena);
  return NULL;
}

void event_manager_create(EventManager *manager) {
  assert_log(manager != NULL, "Manager is NULL");

  manager->arena = arena_create(MB(1), MB(1));
  MemZero(manager->callbacks, sizeof(manager->callbacks));
  manager->queue =
      queue_create_Event(manager->arena, DEFAULT_EVENT_QUEUE_CAPACITY);

  if (!event_data_buffer_create(manager->arena,
                                DEFAULT_EVENT_DATA_RING_BUFFER_CAPACITY,
                                &manager->event_data_buf)) {
    log_fatal("Failed to create event data buffer for EventManager.");
    return;
  }

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

  queue_destroy_Event(&manager->queue);
  event_data_buffer_destroy(&manager->event_data_buf);
  arena_destroy(manager->arena);
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
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(event.type < EVENT_TYPE_MAX, "Invalid event type");
  assert_log(!(event.data_size > 0 && event.data == NULL),
             "Event data is NULL but data_size is greater than 0");

  pthread_mutex_lock(&manager->mutex);

  Event event_to_enqueue = event;
  void *copied_data_ptr_in_buffer = NULL;

  if (event.data_size > 0) {
    if (queue_is_full_Event(&manager->queue)) {
      log_warn("Event queue full. Cannot dispatch event type %d.", event.type);
      pthread_mutex_unlock(&manager->mutex);
      return false;
    }

    if (!event_data_buffer_can_alloc(&manager->event_data_buf,
                                     event.data_size)) {
      log_warn("Event data buffer cannot allocate %llu bytes for event type %d "
               "(full or too fragmented).",
               event.data_size, event.type);
      pthread_mutex_unlock(&manager->mutex);
      return false;
    }

    if (!event_data_buffer_alloc(&manager->event_data_buf, event.data_size,
                                 &copied_data_ptr_in_buffer)) {
      log_warn(
          "Failed to allocate %llu bytes in event data buffer for event type "
          "%d. Allocation failed unexpectedly after can_alloc passed.",
          event.data_size, event.type);
      pthread_mutex_unlock(&manager->mutex);
      return false;
    }

    MemCopy(copied_data_ptr_in_buffer, event.data, event.data_size);

    event_to_enqueue.data = copied_data_ptr_in_buffer;

  } else {
    event_to_enqueue.data = NULL;
    event_to_enqueue.data_size = 0;
  }

  if (!queue_enqueue_Event(&manager->queue, event_to_enqueue)) {
    log_warn("Failed to enqueue event (type: %d). Event queue might be full.",
             event_to_enqueue.type);

    if (event.data_size > 0 && copied_data_ptr_in_buffer != NULL) {
      log_debug("Rolling back event data buffer allocation for event type %d "
                "due to queue enqueue failure.",
                event.type);
      event_data_buffer_rollback_last_alloc(&manager->event_data_buf);
    }

    pthread_mutex_unlock(&manager->mutex);
    return false;
  }

  pthread_cond_signal(&manager->cond);
  pthread_mutex_unlock(&manager->mutex);

  return true;
}
