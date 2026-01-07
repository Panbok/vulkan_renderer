#include "event.h"
#include "memory/vkr_arena_allocator.h"

static bool8_t event_callback_equals(EventCallbackData *current_value,
                                     EventCallbackData *value) {
  return current_value->callback == value->callback &&
         current_value->user_data == value->user_data;
}

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
 * all remaining events in the queue have been drained (graceful shutdown).
 * Cleans up the thread-local arena before exiting.
 * @param arg Pointer to the EventManager instance.
 * @return void* Always returns NULL.
 */
static void *events_processor(void *arg) {
  EventManager *manager = (EventManager *)arg;
  Arena *local_thread_arena = arena_create(KB(64), KB(64));
  VkrAllocator thread_allocator = manager->allocator;
  thread_allocator.ctx = local_thread_arena;
  bool32_t should_run = true;

  while (should_run) {
    Event event;
    Event local_event;
    bool32_t event_dequeued = false;
    bool8_t payload_pending_free = false_v;

    vkr_mutex_lock(manager->mutex);

    while (queue_is_empty_Event(&manager->queue) && manager->running) {
      vkr_cond_wait(manager->cond, manager->mutex);
    }

    should_run = manager->running || !queue_is_empty_Event(&manager->queue);

    if (!queue_is_empty_Event(&manager->queue)) {
      event_dequeued = queue_dequeue_Event(&manager->queue, &event);
      if (event_dequeued && event.data_size > 0 && event.data != NULL) {
        payload_pending_free = true_v;
      }
    }

    if (!event_dequeued) {
      vkr_mutex_unlock(manager->mutex);
      continue;
    }

    if (event.type >= EVENT_TYPE_MAX) {
      log_warn("Processed event with invalid type: %u", event.type);
      if (payload_pending_free) {
        if (!vkr_event_data_buffer_free(&manager->event_data_buf,
                                        event.data_size)) {
          log_error("Events_processor: Failed to free data for event type %d, "
                    "size %llu from event data buffer.",
                    event.type, event.data_size);
        }
      }
      vkr_mutex_unlock(manager->mutex);
      continue;
    }

    Vector_EventCallbackData *callbacks_vec = &manager->callbacks[event.type];
    uint16_t subs_count = callbacks_vec->length;

    if (subs_count == 0) {
      if (payload_pending_free) {
        if (!vkr_event_data_buffer_free(&manager->event_data_buf,
                                        event.data_size)) {
          log_error("Events_processor: Failed to free data for event type %d, "
                    "size %llu from event data buffer.",
                    event.type, event.data_size);
        }
      }
      vkr_mutex_unlock(manager->mutex);
      continue;
    }

    VkrAllocatorScope scope = vkr_allocator_begin_scope(&thread_allocator);
    if (!vkr_allocator_scope_is_valid(&scope)) {
      if (payload_pending_free) {
        if (!vkr_event_data_buffer_free(&manager->event_data_buf,
                                        event.data_size)) {
          log_error("Events_processor: Failed to free data for event type %d, "
                    "size %llu from event data buffer.",
                    event.type, event.data_size);
        }
      }
      vkr_mutex_unlock(manager->mutex);
      continue;
    }

    local_event = event;
    if (payload_pending_free) {
      void *payload_copy = vkr_allocator_alloc(
          &thread_allocator, event.data_size, VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
      if (!payload_copy) {
        log_error("Events_processor: Failed to allocate %llu bytes for event "
                  "type %d payload copy.",
                  event.data_size, event.type);
        if (!vkr_event_data_buffer_free(&manager->event_data_buf,
                                        event.data_size)) {
          log_error("Events_processor: Failed to free data for event type %d, "
                    "size %llu from event data buffer.",
                    event.type, event.data_size);
        }
        vkr_mutex_unlock(manager->mutex);
        vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
        continue;
      }

      MemCopy(payload_copy, event.data, event.data_size);
      local_event.data = payload_copy;

      if (!vkr_event_data_buffer_free(&manager->event_data_buf,
                                      event.data_size)) {
        log_error("Events_processor: Failed to free data for event type %d, "
                  "size %llu from event data buffer.",
                  event.type, event.data_size);
      }
    }

    EventCallbackData *local_callbacks_copy = vkr_allocator_alloc(
        &thread_allocator, (uint64_t)subs_count * sizeof(EventCallbackData),
        VKR_ALLOCATOR_MEMORY_TAG_VECTOR);

    if (callbacks_vec->data != NULL && local_callbacks_copy != NULL) {
      MemCopy(local_callbacks_copy, callbacks_vec->data,
              (size_t)subs_count * sizeof(EventCallbackData));
      vkr_mutex_unlock(manager->mutex);

      for (uint16_t i = 0; i < subs_count; i++) {
        if (local_callbacks_copy[i].callback != NULL) {
          local_callbacks_copy[i].callback(&local_event,
                                           local_callbacks_copy[i].user_data);
        }
      }

    } else {
      log_warn("Event_processor: subs_count (%u) for event type %d, but "
               "vector or array data pointer is NULL. Callbacks skipped.",
               subs_count, event.type);
      vkr_mutex_unlock(manager->mutex);
    }

    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_VECTOR);
  }

  arena_destroy(local_thread_arena);
  return NULL;
}

void event_manager_create(EventManager *manager) {
  assert_log(manager != NULL, "Manager is NULL");

  manager->arena = arena_create(MB(1), MB(1));
  manager->allocator = (VkrAllocator){.ctx = manager->arena};
  if (!vkr_allocator_arena(&manager->allocator)) {
    queue_destroy_Event(&manager->queue);
    arena_destroy(manager->arena);
    log_fatal("Failed to initialize event manager allocator.");
    return;
  }
  MemZero(manager->callbacks, sizeof(manager->callbacks));
  manager->queue =
      queue_create_Event(&manager->allocator, DEFAULT_EVENT_QUEUE_CAPACITY);

  if (!vkr_event_data_buffer_create(&manager->allocator,
                                    DEFAULT_EVENT_DATA_RING_BUFFER_CAPACITY,
                                    &manager->event_data_buf)) {
    queue_destroy_Event(&manager->queue);
    arena_destroy(manager->arena);
    log_fatal("Failed to create event data buffer for EventManager.");
    return;
  }

  vkr_mutex_create(&manager->allocator, &manager->mutex);
  vkr_cond_create(&manager->allocator, &manager->cond);
  manager->running = true;

  vkr_thread_create(&manager->allocator, &manager->thread, events_processor,
                    manager);
}

void event_manager_destroy(EventManager *manager) {
  assert_log(manager != NULL, "Manager is NULL");
  vkr_mutex_lock(manager->mutex);
  manager->running = false;
  vkr_mutex_unlock(manager->mutex);
  vkr_cond_signal(manager->cond);

  vkr_thread_join(manager->thread);
  vkr_thread_destroy(&manager->allocator, &manager->thread);
  vkr_mutex_destroy(&manager->allocator, &manager->mutex);
  vkr_cond_destroy(&manager->allocator, &manager->cond);

  for (uint32_t i = 0; i < EVENT_TYPE_MAX; i++) {
    vector_destroy_EventCallbackData(&manager->callbacks[i]);
  }

  queue_destroy_Event(&manager->queue);
  vkr_event_data_buffer_destroy(&manager->event_data_buf);
  arena_destroy(manager->arena);
}

void event_manager_subscribe(EventManager *manager, EventType type,
                             EventCallback callback, UserData user_data) {
  assert_log(type < EVENT_TYPE_MAX, "Invalid event type");
  assert_log(callback != NULL, "Callback is NULL");
  assert_log(manager != NULL, "Manager is NULL");

  vkr_mutex_lock(manager->mutex);

  if (manager->callbacks[type].data == NULL) {
    manager->callbacks[type] =
        vector_create_EventCallbackData(&manager->allocator);
  }

  uint16_t subs_count = manager->callbacks[type].length;
  for (uint16_t i = 0; i < subs_count; i++) {
    if (manager->callbacks[type].data[i].callback == callback &&
        manager->callbacks[type].data[i].user_data == user_data) {
      log_warn("Callback already subscribed");
      vkr_mutex_unlock(manager->mutex);
      return;
    }
  }

  vector_push_EventCallbackData(&manager->callbacks[type],
                                (EventCallbackData){callback, user_data});
  vkr_mutex_unlock(manager->mutex);
}

void event_manager_unsubscribe(EventManager *manager, EventType type,
                               EventCallback callback) {
  assert_log(type < EVENT_TYPE_MAX, "Invalid event type");
  assert_log(callback != NULL, "Callback is NULL");
  assert_log(manager != NULL, "Manager is NULL");

  vkr_mutex_lock(manager->mutex);
  VectorFindResult res = vector_find_EventCallbackData(
      &manager->callbacks[type], &(EventCallbackData){callback, NULL},
      event_callback_equals);
  if (res.found) {
    vector_pop_at_EventCallbackData(&manager->callbacks[type], res.index, NULL);
  }
  vkr_mutex_unlock(manager->mutex);
}

bool32_t event_manager_dispatch(EventManager *manager, Event event) {
  assert_log(manager != NULL, "Manager is NULL");
  assert_log(event.type < EVENT_TYPE_MAX, "Invalid event type");
  assert_log(!(event.data_size > 0 && event.data == NULL),
             "Event data is NULL but data_size is greater than 0");

  vkr_mutex_lock(manager->mutex);

  Event event_to_enqueue = event;
  void *copied_data_ptr_in_buffer = NULL;

  if (event.data_size > 0) {
    if (queue_is_full_Event(&manager->queue)) {
      log_warn("Event queue full. Cannot dispatch event type %d.", event.type);
      vkr_mutex_unlock(manager->mutex);
      return false;
    }

    if (!vkr_event_data_buffer_can_alloc(&manager->event_data_buf,
                                         event.data_size)) {
      log_warn("Event data buffer cannot allocate %llu bytes for event type %d "
               "(full or too fragmented).",
               event.data_size, event.type);
      vkr_mutex_unlock(manager->mutex);
      return false;
    }

    if (!vkr_event_data_buffer_alloc(&manager->event_data_buf, event.data_size,
                                     &copied_data_ptr_in_buffer)) {
      log_warn(
          "Failed to allocate %llu bytes in event data buffer for event type "
          "%d. Allocation failed unexpectedly after can_alloc passed.",
          event.data_size, event.type);
      vkr_mutex_unlock(manager->mutex);
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
      vkr_event_data_buffer_rollback_last_alloc(&manager->event_data_buf);
    }

    vkr_mutex_unlock(manager->mutex);
    return false;
  }

  vkr_cond_signal(manager->cond);
  vkr_mutex_unlock(manager->mutex);

  return true;
}
