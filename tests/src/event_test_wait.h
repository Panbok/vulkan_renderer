#pragma once

#include "core/event.h"
#include "core/vkr_atomic.h"
#include "platform/vkr_platform.h"

static bool8_t event_test_mark_idle(Event *event, UserData user_data) {
  (void)event;
  vkr_atomic_bool_store(user_data, true_v, VKR_MEMORY_ORDER_RELEASE);
  return true_v;
}

/* A FIFO marker publishes completion of all callbacks queued before it. */
static void event_test_wait_idle(EventManager *manager) {
  VkrAtomicBool completed = false_v;
  const EventType marker_type = (EventType)(EVENT_TYPE_MAX - 1);
  assert(event_manager_subscribe(manager, marker_type, event_test_mark_idle,
                                 &completed));
  assert(event_manager_dispatch(manager, (Event){.type = marker_type}));
  const float64_t deadline = vkr_platform_get_absolute_time() + 30.0;
  while (!vkr_atomic_bool_load(&completed, VKR_MEMORY_ORDER_ACQUIRE)) {
    assert(vkr_platform_get_absolute_time() < deadline &&
           "Event worker did not process the FIFO completion marker");
    vkr_platform_sleep(1u);
  }
  event_manager_unsubscribe(manager, marker_type, event_test_mark_idle);
}
