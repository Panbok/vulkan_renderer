#include "application/vkr_application_host.h"

#include "core/input.h"
#include "platform/vkr_platform.h"

#include <assert.h>
#include <math.h>

vkr_internal bool8_t vkr_application_host_event(Event *event,
                                                UserData user_data) {
  VkrApplicationHost *host = user_data;
  if (!host || !host->callbacks.event)
    return true_v;
  return host->callbacks.event(host->callbacks.state, event);
}

/* Queue before direct delivery so asynchronously subscribed observers retain
 * lifecycle order when a callback causes another lifecycle transition. */
vkr_internal void vkr_application_host_dispatch_lifecycle(
    VkrApplicationHost *host, EventType type) {
  Event event = {.type = type};
  (void)event_manager_dispatch(&host->events, event);
  if (host->callbacks.event)
    host->callbacks.event(host->callbacks.state, &event);
}

vkr_internal bool8_t
vkr_application_host_subscribe_events(VkrApplicationHost *host) {
  const EventType types[] = {
      EVENT_TYPE_WINDOW_CLOSE,         EVENT_TYPE_WINDOW_INIT,
      EVENT_TYPE_WINDOW_RESIZE,        EVENT_TYPE_KEY_PRESS,
      EVENT_TYPE_KEY_RELEASE,          EVENT_TYPE_MOUSE_MOVE,
      EVENT_TYPE_MOUSE_WHEEL,          EVENT_TYPE_BUTTON_PRESS,
      EVENT_TYPE_BUTTON_RELEASE,
  };
  for (uint32_t i = 0u; i < sizeof(types) / sizeof(types[0]); ++i) {
    if (!event_manager_subscribe(&host->events, types[i],
                                 vkr_application_host_event, host))
      return false_v;
  }
  return true_v;
}

bool8_t vkr_application_host_create(VkrApplicationHost *host,
                                    const VkrApplicationHostConfig *config,
                                    VkrAllocator *allocator) {
  if (!host || !config || !allocator || !config->title || config->width == 0u ||
      config->height == 0u || !isfinite(config->fixed_delta_seconds) ||
      config->fixed_delta_seconds < 0.0) {
    return false_v;
  }
  MemZero(host, sizeof(*host));
  host->config = *config;
  host->allocator = allocator;

  bool8_t platform_ready = false_v;
  bool8_t events_ready = false_v;
  bool8_t window_ready = false_v;
  bool8_t mutex_ready = false_v;
  bool8_t gamepad_ready = false_v;
  if (!vkr_platform_init())
    goto cleanup;
  platform_ready = true_v;
  if (!event_manager_create(&host->events))
    goto cleanup;
  events_ready = true_v;
  if (!vkr_application_host_subscribe_events(host))
    goto cleanup;
  if (config->windowed) {
    host->window.hidden = config->window_hidden;
    if (!vkr_window_create(&host->window, &host->events, config->title,
                           config->x, config->y, config->width, config->height))
      goto cleanup;
    window_ready = true_v;
    if (!vkr_gamepad_init(&host->gamepad, &host->window.input_state))
      goto cleanup;
    gamepad_ready = true_v;
  }
  if (!vkr_mutex_create(allocator, &host->mutex))
    goto cleanup;
  mutex_ready = true_v;
  host->clock = vkr_clock_create();
  host->flags = bitset8_create();
  bitset8_set(&host->flags, VKR_APPLICATION_HOST_FLAG_INITIALIZED);
  return true_v;

cleanup:
  if (gamepad_ready)
    vkr_gamepad_shutdown(&host->gamepad);
  if (window_ready)
    vkr_window_destroy(&host->window);
  if (events_ready)
    event_manager_destroy(&host->events);
  if (mutex_ready)
    vkr_mutex_destroy(allocator, &host->mutex);
  if (platform_ready)
    vkr_platform_shutdown();
  MemZero(host, sizeof(*host));
  return false_v;
}

void vkr_application_host_set_callbacks(
    VkrApplicationHost *host, const VkrApplicationHostCallbacks *callbacks) {
  assert(host && callbacks);
  host->callbacks = *callbacks;
}

void vkr_application_host_run(VkrApplicationHost *host) {
  assert(host);
  assert(bitset8_is_set(&host->flags, VKR_APPLICATION_HOST_FLAG_INITIALIZED));
  assert(host->callbacks.frame);
  bitset8_set(&host->flags, VKR_APPLICATION_HOST_FLAG_RUNNING);
  if (!bitset8_is_set(&host->flags,
                      VKR_APPLICATION_HOST_FLAG_INIT_DISPATCHED)) {
    bitset8_set(&host->flags, VKR_APPLICATION_HOST_FLAG_INIT_DISPATCHED);
    vkr_application_host_dispatch_lifecycle(host, EVENT_TYPE_APPLICATION_INIT);
  }
  if (!bitset8_is_set(&host->flags, VKR_APPLICATION_HOST_FLAG_RUNNING) ||
      bitset8_is_set(&host->flags, VKR_APPLICATION_HOST_FLAG_SUSPENDED)) {
    bitset8_clear(&host->flags, VKR_APPLICATION_HOST_FLAG_RUNNING);
    return;
  }
  vkr_clock_start(&host->clock);
  vkr_clock_update(&host->clock);
  host->last_frame_time = host->clock.elapsed;
  const float64_t target_seconds = host->config.target_frame_rate
                                       ? 1.0 / host->config.target_frame_rate
                                       : 0.0;

  bool8_t window_running = true_v;
  while (window_running &&
         bitset8_is_set(&host->flags, VKR_APPLICATION_HOST_FLAG_RUNNING) &&
         bitset8_is_set(&host->flags, VKR_APPLICATION_HOST_FLAG_INITIALIZED)) {
    vkr_clock_update(&host->clock);
    const VkrApplicationHostFrame frame = {
        .delta_seconds = host->config.fixed_delta_seconds > 0.0
                             ? host->config.fixed_delta_seconds
                             : host->clock.elapsed - host->last_frame_time,
        .elapsed_seconds = host->clock.elapsed,
        .absolute_time_seconds = vkr_platform_get_absolute_time(),
    };
    VkrApplicationHostFrame frame_with_delta = frame;
    if (frame_with_delta.delta_seconds > 0.1)
      frame_with_delta.delta_seconds = 0.1;
    if (frame_with_delta.delta_seconds <= 0.0)
      frame_with_delta.delta_seconds =
          target_seconds > 0.0 ? target_seconds : 1.0 / 60.0;

    if (host->config.windowed) {
      window_running = vkr_window_update(&host->window);
      vkr_gamepad_poll_all(&host->gamepad);
    }
    if (!window_running) {
      vkr_application_host_close(host);
      break;
    }
    if (bitset8_is_set(&host->flags, VKR_APPLICATION_HOST_FLAG_SUSPENDED)) {
      host->last_frame_time = frame_with_delta.elapsed_seconds;
      vkr_platform_sleep(1u);
      continue;
    }
    if (!host->callbacks.frame(host->callbacks.state, &frame_with_delta)) {
      vkr_application_host_close(host);
      break;
    }

    if (target_seconds > 0.0) {
      const float64_t elapsed = vkr_platform_get_absolute_time() -
                                frame_with_delta.absolute_time_seconds;
      const float64_t remaining = target_seconds - elapsed;
      if (remaining > 0.0) {
        const uint64_t milliseconds = (uint64_t)(remaining * 1000.0);
        if (milliseconds > 0u) {
          if (host->callbacks.sleep)
            host->callbacks.sleep(host->callbacks.state, milliseconds);
          else
            vkr_platform_sleep(milliseconds);
        }
      }
    }
    if (host->callbacks.frame_complete)
      host->callbacks.frame_complete(host->callbacks.state, &frame_with_delta);
    host->last_frame_time = frame_with_delta.elapsed_seconds;
    if (host->config.windowed)
      input_update(&host->window.input_state);
  }
  bitset8_clear(&host->flags, VKR_APPLICATION_HOST_FLAG_RUNNING);
}

void vkr_application_host_stop(VkrApplicationHost *host) {
  assert(host &&
         bitset8_is_set(&host->flags, VKR_APPLICATION_HOST_FLAG_RUNNING));
  bitset8_set(&host->flags, VKR_APPLICATION_HOST_FLAG_SUSPENDED);
  vkr_application_host_dispatch_lifecycle(host, EVENT_TYPE_APPLICATION_STOP);
}

void vkr_application_host_resume(VkrApplicationHost *host) {
  assert(host &&
         bitset8_is_set(&host->flags, VKR_APPLICATION_HOST_FLAG_SUSPENDED));
  bitset8_clear(&host->flags, VKR_APPLICATION_HOST_FLAG_SUSPENDED);
  vkr_application_host_dispatch_lifecycle(host,
                                          EVENT_TYPE_APPLICATION_RESUME);
}

void vkr_application_host_close(VkrApplicationHost *host) {
  assert(host);
  bitset8_clear(&host->flags, VKR_APPLICATION_HOST_FLAG_RUNNING);
}

void vkr_application_host_shutdown(VkrApplicationHost *host) {
  assert(host);
  assert(!bitset8_is_set(&host->flags, VKR_APPLICATION_HOST_FLAG_RUNNING));
  if (!bitset8_is_set(&host->flags, VKR_APPLICATION_HOST_FLAG_INITIALIZED))
    return;
  bitset8_clear(&host->flags, VKR_APPLICATION_HOST_FLAG_INITIALIZED);
  if (bitset8_is_set(&host->flags, VKR_APPLICATION_HOST_FLAG_INIT_DISPATCHED)) {
    vkr_application_host_dispatch_lifecycle(host,
                                            EVENT_TYPE_APPLICATION_SHUTDOWN);
  }
}

void vkr_application_host_destroy(VkrApplicationHost *host) {
  assert(host);
  assert(!bitset8_is_set(&host->flags, VKR_APPLICATION_HOST_FLAG_RUNNING));
  vkr_application_host_shutdown(host);
  if (host->config.windowed) {
    vkr_gamepad_shutdown(&host->gamepad);
    vkr_window_destroy(&host->window);
  }
  event_manager_destroy(&host->events);
  vkr_mutex_destroy(host->allocator, &host->mutex);
  vkr_platform_shutdown();
  MemZero(host, sizeof(*host));
}
